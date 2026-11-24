#include <ESP8266WiFi.h>
#include <espnow.h>
#include <EEPROM.h>
#include <Wire.h>
#include <time.h>
#include <Servo.h>
extern "C" {
  #include "user_interface.h"
}

// ---------------- PİN TANIMLAMALARI ----------------
#define PIN_BATTERY     A0
#define PIN_SCL         D1
#define PIN_SDA         D2
#define PIN_SERVO       D5
#define PIN_TRIG        D6
#define PIN_ECHO        D7
#define PIN_BUZZER      D8
// NOT: Deep Sleep uyandırması için D0 pini fiziksel olarak RST pinine bağlanmalıdır!
#define DS1307_I2C_ADDRESS 0x68 // RTC için fiziksel I2C adresi

// ---------------- SERVO AYARLARI -------------------
Servo servo;
constexpr int SERVO_MAX     = 2500;
constexpr int SERVO_MIN     = 500;
constexpr int SERVO_CLOSED  = 30;
constexpr int SERVO_OPEN    = 120;
constexpr int OPEN_COVER_DELAY   = 250;
constexpr int COVER_HOLD_MS = 5000; // Kapanistan sonra servo bu sure attach kalir (detach sonrasi kapagi oynatan parazit gerilime karsi), sonra loop() birakir
bool servoHold = false;

// ---------------- MAMA SEVİYESİ --------------------
constexpr int FOOD_MAX_CM   = 3;
constexpr int FOOD_MIN_CM   = 28;

// Ölçüm yöntemi: en fazla FOOD_MAX_ATTEMPTS deneme, FOOD_SAMPLES_NEEDED geçerli örneğin ortalaması
constexpr int           FOOD_SAMPLES_NEEDED    = 5;
constexpr int           FOOD_MAX_ATTEMPTS      = 10;
constexpr unsigned long FOOD_ECHO_TIMEOUT_US   = 5000; // örnek başına ECHO bekleme (5 ms, ~85 cm)
constexpr unsigned long FOOD_SAMPLE_GAP_MS     = 60;   // örnekler arası bekleme
constexpr uint8_t       FOOD_ERROR_PCT         = 101;  // ESP32'ye: ölçüm hatalı / henüz ölçülmedi

// Index 0 = 3 cm (Dolu), Index 25 = 28 cm (Boş)
constexpr float volumeLUT[26] = {
  5.109, 4.825, 4.542, 4.258, 3.975, 3.691, //  3 -  8 cm
  3.408, 3.124, 2.841, 2.557, 2.274, 1.990, //  9 - 14 cm
  1.707, 1.423, 1.158, 0.927, 0.729, 0.561, // 15 - 20 cm
  0.421, 0.305, 0.212, 0.139, 0.084, 0.044, // 21 - 26 cm
  0.017, 0.000                              // 27 - 28 cm
};

// ---------------- YEMLEYİCİ DURUMU -----------------
bool autoFeedOn = false;
bool pendingFeed = false;
bool lastFedThisMinute = false;
unsigned long feedDoneTime  = 0;
unsigned long lastAutoFeedCheck = 0;
struct FeedSlot {
  bool    active;
  uint8_t hour;
  uint8_t minute;
};
FeedSlot slots[4];
uint8_t cachedBatPct   = 0;
uint8_t cachedFoodPct  = FOOD_ERROR_PCT; // 101 = henüz ölçülmedi veya ölçüm hatalı
bool foodMeasurePending = false;
constexpr unsigned long FOOD_MEASURE_DELAY_MS = 2500; // Yemleme sonrası bekleme (ms)

// ---------------- KANAL VE UYKU DEĞİŞKENLERİ ----------------
uint8_t currentChannel = 1;
uint8_t pendingChannel = 0;

// Uyanma sonrası test/komut gönderme penceresi (30 Saniye)
constexpr unsigned long WAKE_GRACE_PERIOD_MS = 30000UL; 
unsigned long bootMillis = 0;

bool deepSleepOn = false;
uint8_t dsStartH = 23;
uint8_t dsStartM = 0;
uint8_t dsEndH   = 7;
uint8_t dsEndM   = 0;

// ---------------- EEPROM BELLEK HARİTASI ----------------
#define EEPROM_SIZE          32  
#define ADDR_AUTO            0
#define ADDR_SLOTS           1    // 12 byte kaplar (1'den 12'ye)
#define ADDR_CHANNEL         13
#define ADDR_SLEEP_ON        14
#define ADDR_SLEEP_SH        15
#define ADDR_SLEEP_SM        16
#define ADDR_SLEEP_EH        17
#define ADDR_SLEEP_EM        18

// ---------------- EEPROM YARDIMCI FONKSİYONLARI ----------------
void saveToEEPROM() {
  EEPROM.write(ADDR_AUTO, autoFeedOn ? 1 : 0);
  for (int i = 0; i < 4; i++) {
    int base = ADDR_SLOTS + i * 3;
    EEPROM.write(base,     slots[i].active ? 1 : 0);
    EEPROM.write(base + 1, slots[i].hour);
    EEPROM.write(base + 2, slots[i].minute);
  }
  EEPROM.write(ADDR_CHANNEL, currentChannel);
  EEPROM.write(ADDR_SLEEP_ON, deepSleepOn ? 1 : 0);
  EEPROM.write(ADDR_SLEEP_SH, dsStartH);
  EEPROM.write(ADDR_SLEEP_SM, dsStartM);
  EEPROM.write(ADDR_SLEEP_EH, dsEndH);
  EEPROM.write(ADDR_SLEEP_EM, dsEndM);
  EEPROM.commit();
}

void loadFromEEPROM() {
  autoFeedOn = (EEPROM.read(ADDR_AUTO) == 1);
  for (int i = 0; i < 4; i++) {
    int base = ADDR_SLOTS + i * 3;
    slots[i].active = (EEPROM.read(base) == 1);
    slots[i].hour   = EEPROM.read(base + 1);
    slots[i].minute = EEPROM.read(base + 2);
    if (slots[i].hour > 23) slots[i].hour = 0;
    if (slots[i].minute > 59) slots[i].minute = 0;
  }
  
  currentChannel = EEPROM.read(ADDR_CHANNEL);
  if (currentChannel < 1 || currentChannel > 13) currentChannel = 1;

  deepSleepOn = (EEPROM.read(ADDR_SLEEP_ON) == 1);
  dsStartH    = EEPROM.read(ADDR_SLEEP_SH);
  dsStartM    = EEPROM.read(ADDR_SLEEP_SM);
  dsEndH      = EEPROM.read(ADDR_SLEEP_EH);
  dsEndM      = EEPROM.read(ADDR_SLEEP_EM);
  
  if (dsStartH > 23) dsStartH = 23;
  if (dsStartM > 59) dsStartM = 0;
  if (dsEndH > 23) dsEndH = 7;
  if (dsEndM > 59) dsEndM = 0;
}

// ---------------- BUZZER FONKSİYONLARI ----------------
void bootBeep()     { for (int i = 0; i < 1; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(200); digitalWrite(PIN_BUZZER, LOW); delay(100); } }
void successBeep()  { for (int i = 0; i < 3; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(50);  digitalWrite(PIN_BUZZER, LOW); delay(50);  } }
void failBeep()     { for (int i = 0; i < 3; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(200); digitalWrite(PIN_BUZZER, LOW); delay(100); } }
void coverBeep()    { for (int i = 0; i < 2; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(100); digitalWrite(PIN_BUZZER, LOW); delay(100); } }

// ---------------- ZAMAN YÖNETİMİ -------------------
constexpr uint32_t RTC_INTERVAL = 60 * 60 * 1000UL;

bool rtcSyncedByNTP = false;
bool timeSyncedByRTC = false;
unsigned long lastSyncTimeByRTC = 0;

uint8_t decToBcd(uint8_t val) { return ((val / 10 * 16) + (val % 10)); }
uint8_t bcdToDec(uint8_t val) { return ((val / 16 * 10) + (val % 16)); }

// ---------------- RTC FONKSİYONLARI ----------------
bool readRTC(uint32_t &timestamp) {
  Wire.beginTransmission(DS1307_I2C_ADDRESS); Wire.write(0x00); 
  if (Wire.endTransmission() != 0) return false; 
  Wire.requestFrom((uint8_t)DS1307_I2C_ADDRESS, (uint8_t)7);
  if (Wire.available() < 7) return false;
  uint8_t seconds = bcdToDec(Wire.read() & 0x7F); 
  uint8_t minutes = bcdToDec(Wire.read());
  uint8_t hours   = bcdToDec(Wire.read() & 0x3F);
  uint8_t wday    = bcdToDec(Wire.read());
  uint8_t mday    = bcdToDec(Wire.read());
  uint8_t month   = bcdToDec(Wire.read());
  uint16_t year   = bcdToDec(Wire.read());

  struct tm timeinfo = {};
  timeinfo.tm_sec   = seconds; 
  timeinfo.tm_min   = minutes; 
  timeinfo.tm_hour  = hours; 
  timeinfo.tm_mday  = mday; 
  timeinfo.tm_mon   = month - 1;  // tm_mon: 0-11
  timeinfo.tm_year  = year + 100; // tm_year: +2000 -1900 > 1900'den beri geçen yıl

  time_t rawTime = mktime(&timeinfo);
  if (rawTime != -1) {
    timestamp = (uint32_t)rawTime;
    return true;
  }
  return false;   // Geçersiz tarih dönüşüm hatası
}

bool writeRTC(uint32_t timestamp) {
  time_t rawTime = (time_t)timestamp;
  struct tm timeinfo;
  localtime_r(&rawTime, &timeinfo);

  Wire.beginTransmission(DS1307_I2C_ADDRESS); 
  Wire.write(0x00); // Yazmaya 0x00 (Saniye) adresinden başla
  
  Wire.write(decToBcd(timeinfo.tm_sec));        // 0x00: Saniye
  Wire.write(decToBcd(timeinfo.tm_min));        // 0x01: Dakika
  Wire.write(decToBcd(timeinfo.tm_hour));       // 0x02: Saat
  Wire.write(decToBcd(timeinfo.tm_wday + 1));   // 0x03: Haftanın günü (1=Pazar .. 7=Cumartesi)
  Wire.write(decToBcd(timeinfo.tm_mday));       // 0x04: Ayın Günü (1-31)
  Wire.write(decToBcd(timeinfo.tm_mon + 1));    // 0x05: Ay (1-12)
  Wire.write(decToBcd(timeinfo.tm_year - 100)); // 0x06: Yıl (00-99) (Örn: 2026 için 26)
  
  return (Wire.endTransmission() == 0);
}

bool updateSystemTimeFromRTC() {
  rtcSyncedByNTP = false;
  lastSyncTimeByRTC = millis();
  uint32_t rtcTimestamp = 0;
  Serial.println("[SYSTEM] Saat RTC ile guncelleniyor...");

  if (readRTC(rtcTimestamp)) {
    time_t t = (time_t)rtcTimestamp;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, nullptr); 
    Serial.printf("[RTC: OK] RTC basarili! SYSTEM saati guncellendi: %s", ctime(&t));
    successBeep(); 
    return timeSyncedByRTC = true;
  } else {
    time_t now = time(nullptr);
    Serial.printf("[RTC: ERR] RTC okunamadi! Mevcut SYSTEM saati: %s", ctime(&now));
    failBeep(); 
    return timeSyncedByRTC = false;
  }
}

// ---------------- SENSÖR & KONTROL FONKSİYONLARI ----------------

// Mama mesafesi ölçümü: her örnek için TRIG darbesi, ECHO en fazla FOOD_ECHO_TIMEOUT_US beklenir.
// Geçerli örnek = yankı gelmiş ve 1-30 cm arasında. FOOD_SAMPLES_NEEDED geçerli örneğin ORTALAMASI alınır.
// FOOD_SAMPLES_NEEDED geçerli örnek tamamlanınca durur; başarısızlık sayısı (FOOD_MAX_ATTEMPTS - FOOD_SAMPLES_NEEDED)'i
// aşınca da durur (kalan denemeler yeterli geçerli örnek veremez). Başarısızsa -1 döner.
float getFoodDistance() {
  float sum = 0.0f, minD = 999.0f, maxD = 0.0f;
  int ok = 0, fail = 0, busy = 0, tries = 0;

  for (int i = 0; i < FOOD_MAX_ATTEMPTS; i++) {
    if (i > 0) delay(FOOD_SAMPLE_GAP_MS);
    tries++;
    if (digitalRead(PIN_ECHO)) busy++;  // teşhis: ECHO önceki örnekten hâlâ HIGH mı

    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);

    long duration = pulseIn(PIN_ECHO, HIGH, FOOD_ECHO_TIMEOUT_US);
    float distance = (duration > 0) ? (duration * 0.0343f) / 2.0f : -1.0f;

    if (distance >= 1.0f && distance <= 30.0f) {
      sum += distance; ok++;
      if (distance < minD) minD = distance;
      if (distance > maxD) maxD = distance;
      if (ok >= FOOD_SAMPLES_NEEDED) break;
    } else {
      fail++;
      if (fail > FOOD_MAX_ATTEMPTS - FOOD_SAMPLES_NEEDED) break;
    }
  }

  if (ok >= FOOD_SAMPLES_NEEDED) {
    float avg = sum / ok;
    Serial.printf("[FOOD] deneme=%d gecerli=%d basarisiz=%d echo-busy=%d min=%.1f max=%.1f ort=%.2f cm\n", tries, ok, fail, busy, minD, maxD, avg);
    return avg;
  }
  Serial.printf("[FOOD] OLCUM BASARISIZ deneme=%d gecerli=%d basarisiz=%d echo-busy=%d\n", tries, ok, fail, busy);
  return -1.0f;
}

// Lookup tablosu ve interpolasyon ile hacmi hesaplayan fonksiyon
float calculateVolume(float distance) {
  if (distance <= FOOD_MAX_CM) return volumeLUT[0];  // 3 cm veya altı -> %100 Dolu
  if (distance >= FOOD_MIN_CM) return volumeLUT[25]; // 28 cm veya üstü -> %0 (Boş)
  float offsetDistance = distance - FOOD_MAX_CM;
  // Doğrusal interpolasyon işlemi
  int lowerIndex = (int)offsetDistance;             // Örn: Mesafe 14.7 cm ise alt indeks 14
  int upperIndex = lowerIndex + 1;                  // Üst indeks 15
  float fraction = offsetDistance - lowerIndex;     // Kesirli kısım: 14.7 - 14 = 0.7

  // Formül: Alt Değer + (Kesirli Kısım * (Üst Değer - Alt Değer))
  float volume = volumeLUT[lowerIndex] + (fraction * (volumeLUT[upperIndex] - volumeLUT[lowerIndex]));
  return volume;
}

// Ana okuma ve % doluluk döndürme fonksiyonu. Ölçüm başarısızsa FOOD_ERROR_PCT (101) döner;
// ESP32 bunu hatalı ölçüm sayar ve eski mama değerini korur.
int readFoodLevel() {
  float measuredDistance = getFoodDistance();
  digitalWrite(PIN_TRIG, LOW);
  if (measuredDistance < 0.0f) return FOOD_ERROR_PCT;

  // Mesafeyi litreye çevir
  float currentVolume = calculateVolume(measuredDistance);

  // Doluluk yüzdesini hesapla (Maksimum hacim volumeLUT[0]'dır)
  float fillPercentage = (currentVolume / volumeLUT[0]) * 100.0f;
  int result = (int)(fillPercentage + 0.5f);
  if (result < 0)   result = 0;
  if (result > 100) result = 100;
  Serial.printf("[FOOD] -> %%%d\n", result);
  return result;
}

int readBattPercent() {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(PIN_BATTERY); delay(10); }
  int raw = sum / 10;
  float voltage = (raw / 1023.0f) * 5.60695f;
  static const float voltTable[] = { 3.00, 3.10, 3.20, 3.30, 3.40, 3.50, 3.60, 3.75, 3.90, 4.05, 4.15 };
  static const int   pctTable[]  = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
  if (voltage <= voltTable[0]) return 0;
  if (voltage >= voltTable[10]) return 100;
  for (int i = 0; i < 10; i++) {
    if (voltage >= voltTable[i] && voltage <= voltTable[i+1]) {
      float ratio = (voltage - voltTable[i]) / (voltTable[i+1] - voltTable[i]);
      return (int)(pctTable[i] + ratio * (pctTable[i+1] - pctTable[i]));
    }
  }
  return 0;
}

void openCover() { 
  // servo attach/detach loop()'ta yapilir; burasi yalniz ac/kapa
  for (int i = 0; i < 2; i++) { coverBeep(); delay(200); };
  servo.write(SERVO_OPEN);
  delay(OPEN_COVER_DELAY);
  servo.write(SERVO_CLOSED);
}

void feedNow() {
  lastFedThisMinute = true;
  openCover();
  feedDoneTime = millis();
  foodMeasurePending = true;
}

void checkAutoFeed() {
  if (!autoFeedOn) return;
  time_t now = time(NULL); struct tm *t = localtime(&now);
  int h = t->tm_hour; int m = t->tm_min; int s = t->tm_sec;
  bool onTime = false;
  for (int i = 0; i < 4; i++) {
    if (slots[i].active && slots[i].hour == h && slots[i].minute == m && s < 55) { onTime = true; break; }
  }
  if (onTime && !lastFedThisMinute) pendingFeed = true; 
  else if (!onTime) lastFedThisMinute = false;
}

// ---------------- ------ ESP-NOW ------ ---------------- //

// ---------------- ESP-NOW KOMUT TİPLERİ ---------------- //
enum CommandType {
  CMD_PING          = 1,
  CMD_SET_CHANNEL   = 2,
  CMD_MANUAL_FEED   = 3,
  CMD_REQ_STATUS    = 4,  // pil seviyesi + yem seviyesi birlikte gönderilecek
  CMD_SET_CONFIG    = 5,  // yemleme slotları + uyku ayarı birlikte gelecek
  CMD_REQ_CONFIG    = 6,  // yemleme slotları + uyku ayarı birlikte gönderilecek
  CMD_SET_RTC       = 7,  // ntp saati ile rtc'yi kurmak için timestamp gelecek
  CMD_REQ_RTC       = 8   // ntp saatini alamadığında rtc'den timestamp gönderilecek
};

// ---------------- VERİ PAKETİ YAPISI ----------------
typedef struct __attribute__((packed)) {
  uint8_t  command;   // ESP-NOW Komut numarası
  uint8_t  values[4]; // Genel amaçlı değerler [0]=isACK, [1]=doesAsyncAwait, [2]=mainValue, [3]= secondValue
  uint8_t  slot_h[4]; // Otomatik yemleme zamanı saat bilgileri slot 1/2/3/4
  uint8_t  slot_m[4]; // Otomatik yemleme zamanı dakika bilgileri slot 1/2/3/4
  uint8_t  sleep[4];  // Deep Seleep zamanı [0]=başSaat, [1]=başDk, [2]=bitişSaat, [3]=bitişDk
  uint8_t  flags;     // Conf flags bit0-3: slot aktif, bit4: autoEnabled, bit5: sleepEnabled
  uint32_t timestamp; // Hem benzersiz komut işlemi işin hem de rtc set/req için kullanılır
} EspNowMsg;

#define FEEDER_FLAG_GET(flags, bit)      (((flags) >> (bit)) & 1)
#define FEEDER_FLAG_SET(flags, bit, val) ((val) ? ((flags) |= (1<<(bit))) : ((flags) &= ~(1<<(bit))))

// ESP-NOW Burst koruması için değişkenler
const unsigned long BURST_IGNORE_WINDOW = 500; // 500 ms içindeki kopyaları yoksay
unsigned long lastCommandTime = 0;
bool pendingReply = false;
bool pendingAsyncReply = false;
uint8_t replyMac[6];
EspNowMsg receivedMsg;

// --- ESP-NOW CEVAPLARINI ÇOKLU (BURST) GÖNDERME FONKSİYONU ---
void sendEspNowRespond() {
  if (!esp_now_is_peer_exist(replyMac)) {
    esp_now_add_peer(replyMac, ESP_NOW_ROLE_COMBO, currentChannel, NULL, 0);
  }
  EspNowMsg sendingMsg = {};
  sendingMsg.command = receivedMsg.command;
  sendingMsg.values[0] = 1; // varsayılan olarak ack göndereceğiz
  sendingMsg.timestamp = (uint32_t)time(NULL);

  switch (receivedMsg.command) {
    case CMD_PING: {
      // ping için sadece ack mesajı dönecek, başka işlem yapmıyoruz
      break;
    }
    case CMD_SET_CHANNEL: {
      // kanal değiştirme için reset yapılacak, bu yüzden önce ack gönderip asıl işi loop'ta yapacağız
      pendingChannel = receivedMsg.values[2];
      break;
    }
    case CMD_MANUAL_FEED: {
      // feedNow işlemi zaman alıyor, bu yüzden önce ack gönderip asıl işi loop'ta yapacağız
      pendingFeed = true;
      break;
    }
    case CMD_REQ_STATUS: {
      sendingMsg.values[0] = 0; // bu komut nitelikli yanıt bekliyor, ack değil
      sendingMsg.values[2] = cachedBatPct; // pil ölçümü rtc ile periyodik cache yapılır
      sendingMsg.values[3] = cachedFoodPct; // mama ölçümü feedNow sonrası cache yapılır
      break;
    }
    case CMD_SET_CONFIG: {
      for (int i = 0; i < 4; i++) {
        slots[i].hour   = receivedMsg.slot_h[i];
        slots[i].minute = receivedMsg.slot_m[i];
        slots[i].active = FEEDER_FLAG_GET(receivedMsg.flags, i);
      }
      autoFeedOn = FEEDER_FLAG_GET(receivedMsg.flags, 4);
      deepSleepOn = FEEDER_FLAG_GET(receivedMsg.flags, 5);
      dsStartH = receivedMsg.sleep[0];
      dsStartM = receivedMsg.sleep[1];
      dsEndH   = receivedMsg.sleep[2];
      dsEndM   = receivedMsg.sleep[3];
      saveToEEPROM();
      break;
    }
    case CMD_REQ_CONFIG: {
      sendingMsg.values[0] = 0; // bu komut nitelikli yanıt bekliyor, ack değil
      for (int i = 0; i < 4; i++) {
        sendingMsg.slot_h[i]  = slots[i].hour;
        sendingMsg.slot_m[i]  = slots[i].minute;
        FEEDER_FLAG_SET(sendingMsg.flags, i, slots[i].active);
      }
      FEEDER_FLAG_SET(sendingMsg.flags, 4, autoFeedOn);
      FEEDER_FLAG_SET(sendingMsg.flags, 5, deepSleepOn);
      sendingMsg.sleep[0] = dsStartH;
      sendingMsg.sleep[1] = dsStartM;
      sendingMsg.sleep[2] = dsEndH;
      sendingMsg.sleep[3] = dsEndM;
      break;
    }
    case CMD_SET_RTC: {
      // master ntp saatini gönderdi, rtc'yi yazıp ntp bayrağını kaldırıyoruz
      // ack gönderildikten sonra loop'ta SYSTEM saatini rtc ile düzeltip bayrağı indireceğiz
      writeRTC(receivedMsg.timestamp);
      rtcSyncedByNTP = true;
      break;
    }
    case CMD_REQ_RTC: {
      sendingMsg.values[0] = 0; // bu komut nitelikli yanıt bekliyor, ack değil
      // SYSTEM saati periyodik olarak rtc ile düzeltildiği için yerel timestamp yeterli
      // rtc'yi ayrıca okuyup tekrar timestamp yazmaya çalışmıyoruz
      break;
    }
  }

  // gönderdiğimiz mesajın master'a ulaştığından emin olmak için burst yapacağız
  // master'dan ack beklemiyoruz, 3 mesaj atıp işimize bakacağız
  for (int i = 0; i < 3; i++) {
    esp_now_send(replyMac, (uint8_t *)&sendingMsg, sizeof(sendingMsg));
    delay(10);
  }
  pendingReply = false;
}

// ---------------- ESP-NOW CALLBACK ----------------
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  // 1. Zaten bekleyen işlem var mı veya veri geçerli bir msg paketi mi (uzunluğu kontrol et)
  if (pendingReply == true || len != sizeof(EspNowMsg)) return;

  // 2. Gelen veriyi geçici bir yapıya kopyala
  EspNowMsg incomingMsg;
  memcpy(&incomingMsg, incomingData, len);

  unsigned long now = millis();

  // 3. Burst Deduplication
  if (now - lastCommandTime < BURST_IGNORE_WINDOW) {
    // Mesaj aynı (burst) mı diye command + timestamp kontrol et
    if (incomingMsg.command + incomingMsg.timestamp == receivedMsg.command + receivedMsg.timestamp) {
      // pendingReply = true; // İşlemi tekrarlama ama CEVABI tekrar yolla
      return; 
    }
  }

  // 4. Yeni komutu kaydet
  lastCommandTime = now;
  memcpy(&receivedMsg, &incomingMsg, len);
  memcpy(replyMac, mac, 6);
  pendingReply = true; // yeni mesaja yeni bir yanıt gönderilmeli
}

// ---------------- GÜÇ YÖNETİMİ & LIGHT SLEEP ----------------
constexpr unsigned long LISTEN_WINDOW_MS = 50; // Uyanık kalıp ESP-NOW dinleme süresi (ms)
constexpr unsigned long SLEEP_WINDOW_MS  = 450; // Zorunlu Light Sleep süresi (ms)
unsigned long lastWakeTime = 0;

// Wi-Fi ve ESP-NOW Subsystem'ini Güvenli Başlatma/Duraklatma Yardımcısı
void setWifiState(bool enable) {
  if (enable) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    wifi_promiscuous_enable(1);
    wifi_set_channel(currentChannel);
    wifi_promiscuous_enable(0);
    
    if (esp_now_init() == 0) {
      esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
      esp_now_register_recv_cb(OnDataRecv);
    }
  } else {
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
  }
}

// Zorlamalı (Forced) Light Sleep Fonksiyonu
void executeForcedLightSleep(uint32_t sleepTimeMs) {
  // 1. Wi-Fi ve ESP-NOW'ı tamamen durdur
  esp_now_deinit();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin(); // PHY Katmanını kapatır
  delay(1);

  digitalWrite(PIN_TRIG, LOW);
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_SERVO, LOW);

  // 2. FPM Modunu Ayarla ve Uyu
  wifi_fpm_set_sleep_type(LIGHT_SLEEP_T);
  wifi_fpm_open();
  wifi_fpm_do_sleep(sleepTimeMs * 1000UL); 
  
  // İşlemcinin uykuda kalmasını sağla
  delay(sleepTimeMs + 1); 
  
  // 3. Uykudan Çıkış ve Wi-Fi'ı Uyandırma
  wifi_fpm_close();
  WiFi.forceSleepWake();
  delay(1);
  
  // Wi-Fi ve ESP-NOW'ı tekrar aktif et
  setWifiState(true);
}

// Derin uyku kontrol fonkiyonu
void goodNightBabe() {
  time_t nowTime = time(NULL);
  struct tm *t = localtime(&nowTime);
  int currentMins = t->tm_hour * 60 + t->tm_min;
  int startMins   = dsStartH * 60 + dsStartM;
  int endMins     = dsEndH * 60 + dsEndM;
  
  bool shouldSleep = false;
  int sleepDurationMins = 0;

  // Aynı gün içindeyse (Örn: 10:00 - 15:00)
  if (startMins < endMins) {
    if (currentMins >= startMins && currentMins < endMins) {
      shouldSleep = true;
      sleepDurationMins = endMins - currentMins;
    }
  } 
  // Gece yarısını geçiyorsa (Örn: 23:00 - 07:00)
  else if (startMins > endMins) {
    if (currentMins >= startMins || currentMins < endMins) {
      shouldSleep = true;
      if (currentMins >= startMins) sleepDurationMins = (1440 - currentMins) + endMins;
      else sleepDurationMins = endMins - currentMins;
    }
  }
  // Cihaz uyanana kadar buradan sonraki kodlar çalışmaz
  if (shouldSleep && sleepDurationMins > 0) {
    int clampedMins = min(sleepDurationMins, 150); // overflow clamp Max 2.5 saat (150 dk)
    Serial.printf("[SYSTEM] Uyku moduna geciliyor. Sure: %d dakika\n", clampedMins);
    bootBeep();
    uint64_t sleepUs = (uint64_t)clampedMins * 60 * 1000000ULL;
    ESP.deepSleep(sleepUs); 
  }
}

// ---------------- KURULUM ----------------
void setup() {
  Serial.begin(115200); Wire.begin(PIN_SDA, PIN_SCL); EEPROM.begin(EEPROM_SIZE);

  pinMode(PIN_BUZZER, OUTPUT); pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_SERVO, OUTPUT);
  pinMode(PIN_ECHO, INPUT); pinMode(PIN_BATTERY, INPUT); 
  digitalWrite(PIN_BUZZER, LOW); digitalWrite(PIN_TRIG, LOW);

  servo.attach(PIN_SERVO, SERVO_MIN, SERVO_MAX); 
  servo.write(SERVO_CLOSED);

  bootMillis = millis();
  Serial.println("[SYSTEM] ESP8266 Feeder modulu baslatiliyor..."); 
  bootBeep();
  loadFromEEPROM();

  setenv("TZ", "TRT-3", 1); tzset(); updateSystemTimeFromRTC();

  // FPM (Forced Power Save) altyapısını hazırla
  wifi_set_sleep_type(LIGHT_SLEEP_T);

  // Wi-Fi ve ESP-NOW'u modüler fonksiyon üzerinden başlat
  setWifiState(true);
  Serial.printf("[SYSTEM] WIFI/ESP-NOW baslatildi | Channel: %d \n", currentChannel);

  servoHold = true;
  feedDoneTime = bootMillis;
  foodMeasurePending = true;
  cachedBatPct  = (uint8_t)readBattPercent();
}

// ---------------- ANA DÖNGÜ ----------------
void loop() {
  unsigned long now = millis();

  // esp-now cevap işleyicisi
  if (pendingReply) sendEspNowRespond();

  // kanal senkronizasyonu için kanal değişim talebini kontrol et
  if (pendingChannel > 0) {
    delay(100); // master ack mesajını alsın diye bekle
    currentChannel = pendingChannel;
    EEPROM.write(ADDR_CHANNEL, currentChannel);
    EEPROM.commit();
    ESP.restart(); // kanalı sağlıklı değiştirmek için reset yap
  }

  // beslemeyi kontrol et
  if (autoFeedOn && now - lastAutoFeedCheck >= 1000) { lastAutoFeedCheck = now; checkAutoFeed(); }
  if (pendingFeed) {
    pendingFeed = false;
    servo.attach(PIN_SERVO, SERVO_MIN, SERVO_MAX);
    feedNow();
    servoHold = true;
    now = millis(); // feed işlemi, beep ile birlikte yaklaşık 1450ms sürüyor, cover hold ve food measure için now'ı güncelle
  }
  // Kapanistan sonra servoyu COVER_HOLD_MS boyunca attach tut, sonra bırak
  if (servoHold && now - feedDoneTime >= COVER_HOLD_MS) {
    servo.detach();
    digitalWrite(PIN_SERVO, LOW);
    servoHold = false;
  }

  // mama ölçümü yemleme sonrası asenkron yap, soran olursa önbellek gönder
  if (foodMeasurePending && now - feedDoneTime >= FOOD_MEASURE_DELAY_MS) {
    foodMeasurePending = false;
    cachedFoodPct = (uint8_t)readFoodLevel();
  }
  
  // pil ölçümünü rtc senkronizasyonu ile birlikte yap, soran olursa önbellek gönder
  if (rtcSyncedByNTP || now - lastSyncTimeByRTC >= RTC_INTERVAL) {
    updateSystemTimeFromRTC();
    cachedBatPct = (uint8_t)readBattPercent();
  }

  // --- DÖNGÜSEL (DUTY-CYCLED) HİBRİT SLEEP YÖNETİMİ ---
  bool isBusy = pendingReply || pendingFeed || foodMeasurePending || servoHold || (pendingChannel > 0);
  bool inGracePeriod = (now - bootMillis < WAKE_GRACE_PERIOD_MS);

  if (!isBusy && !inGracePeriod) {

    // --- DEEP SLEEP ---
    if (timeSyncedByRTC && deepSleepOn) {
      goodNightBabe();
    }

    // --- LIGHT SLEEP ---
    if (now - lastWakeTime >= LISTEN_WINDOW_MS) {
      executeForcedLightSleep(SLEEP_WINDOW_MS);
      lastWakeTime = millis();
    }
  }
}
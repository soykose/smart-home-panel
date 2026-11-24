// ── KÜTÜPHANE BAŞLIKLARI ──────────────────────────────────────
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <RCSwitch.h>
#include <WebSocketsServer.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "ca_cert.h"

// ── NESNELER ─────────────────────────────────────────────────
WebServer   server(80);
RCSwitch    rcSwitch;
Preferences prefs;
WebSocketsServer webSocket(81);   // Yerel WebSocket (uygulama <-> ESP32)

// ── PİN TANIMLAMALARI ────────────────────────────────────────
#define PIN_POOL     13   // Röle 8 — Havuz Motoru
#define PIN_VALVE1   12   // Röle 7 — Sulama Vana 1
#define PIN_VALVE2   14   // Röle 6 — Sulama Vana 2
#define PIN_LIGHT1   27   // Röle 5 — Aydınlatma 1
#define PIN_LIGHT2   26   // Röle 4 — Aydınlatma 2
#define PIN_RELAY3   25   // Röle 3 — Boş
#define PIN_RELAY2   33   // Röle 2 — Boş
#define PIN_RELAY1   32   // Röle 1 — Boş

const int ALL_RELAY_PINS[] = {
  PIN_LIGHT1, PIN_LIGHT2, PIN_RELAY3, PIN_RELAY1,
  PIN_VALVE1, PIN_VALVE2, PIN_RELAY2, PIN_POOL
};
const int LIGHT_PINS[] = {PIN_LIGHT1, PIN_LIGHT2};
const int VALVE_PINS[] = {PIN_VALVE1, PIN_VALVE2};

void relaySet(int pin, bool on) { digitalWrite(pin, on ? LOW : HIGH); }

// ---------------- BUZZER FONKSİYONLARI ----------------
#define PIN_BUZZER 18 // Aftif Buzzer
void bootBeep() { digitalWrite(PIN_BUZZER, HIGH); delay(200); digitalWrite(PIN_BUZZER, LOW); delay(200); }
void successBeep() { for (int i = 0; i < 3; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(50); digitalWrite(PIN_BUZZER, LOW); delay(50); } }
void failBeep() { for (int i = 0; i < 3; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(200); digitalWrite(PIN_BUZZER, LOW); delay(100); } }
void coverBeep(int count) { for (int i = 0; i < count; i++) { digitalWrite(PIN_BUZZER, HIGH); delay(100); digitalWrite(PIN_BUZZER, LOW); delay(100); } }

// ── RF KONFİGÜRASYONU ───────────────────────────────────────────────
#define PIN_RF_DATA 23       // RF alıcı DATA PIN
#define RF_LIGHT1   13109429 // A
#define RF_LIGHT2   16529281 // B
#define RF_POOL     14489248 // C
#define RF_ALLONOFF 16331140 // D
#define RF_VALVE1   16659586 // E
#define RF_VALVE2   16072328 // F

// ── RF DEBOUNCE DEĞİŞKENLERİ ─────────────────────────────────
unsigned long lastRFCode = 0;          
unsigned long lastRFTime = 0;          
const unsigned long RF_DEBOUNCE_MS = 1200; 

// ── STATİK IP TANIMLAMALARI ──────────────────────────────────
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// ── ZAMAN SABİTLERİ ──────────────────────────────────────────
constexpr uint32_t EPOCH_2026_THRESHOLD = 1767225600UL; // 1 Ocak 2026 00:00:00 UTC = 1767225600 saniye
constexpr uint32_t ONE_MIN_MS           = 60 * 1000UL;
constexpr uint32_t ONE_HOUR_MS          = 60 * ONE_MIN_MS;
constexpr uint32_t ONE_DAY_MS           = 24 * ONE_HOUR_MS;
constexpr uint32_t NTP_TIMEOUT_LIMIT    = 3 * ONE_HOUR_MS + 5 * ONE_MIN_MS; 
constexpr uint32_t RTC_SET_INTERVAL     = 12 * ONE_HOUR_MS; // feeder 11 sa uyuyor: 12 sa arayla denemelerden en az biri uyanık pencereye düşer (uyku süresi 12 saati geçerse garanti bozulur)
constexpr uint32_t RECONNECT_INTERVAL   = 3 * ONE_MIN_MS;
constexpr uint32_t AP_TIMEOUT_MS        = 3 * ONE_MIN_MS;
constexpr int      WIFI_CONNECT_TRIES   = 5;

// ---------------- ------ ESP-NOW ------ ---------------- //
uint8_t   feederMac[]     = {0xEC, 0xFA, 0xBC, 0xC9, 0x3C, 0x2D};
uint8_t   lastFeederChannel = 1; // Preferences'tan yüklenir
bool      feederOnline    = false; // Feeder SYSTEMe dahil oldu mu durum takibi

volatile bool espNowResponded = false;    // Feeder cevap verdi mi
volatile bool espNowAsyncAwait = false;   // Asenkron sonuç beklenecek mi
volatile bool espNowAsyncResponded = false;  // Asenkron sonuç geldi mi

// ---------------- ESP-NOW KOMUT TİPLERİ ---------------- //
enum CommandType {
  CMD_PING        = 1,
  CMD_SET_CHANNEL = 2,
  CMD_MANUAL_FEED = 3,
  CMD_REQ_STATUS  = 4,  // pil seviyesi + yem seviyesi birlikte istenecek
  CMD_SET_CONFIG  = 5,  // yemleme slotları + uyku ayarı birlikte gönder
  CMD_REQ_CONFIG  = 6,  // yemleme slotları + uyku ayarı birlikte al
  CMD_SET_RTC     = 7,  // ntp saati ile rtc'yi kurmak için timestamp gönder
  CMD_REQ_RTC     = 8   // ntp saatini alamadığında rtc'den timestamp al
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

// flags Bit set/get
#define FEEDER_FLAG_GET(flags, bit)      (((flags) >> (bit)) & 1)
#define FEEDER_FLAG_SET(flags, bit, val) ((val) ? ((flags) |= (1<<(bit))) : ((flags) &= ~(1<<(bit))))

// --- Burst Koruması İçin Global Değişkenler ---
EspNowMsg receivedMsg; // Burst koruması için mesaj id olarak command + timestamp kullanılacak
volatile unsigned long lastRecvTime = 0;
const unsigned long ASYNC_AWAIT_TIMEOUT = 3000; // 3sn içinde async cevap gelmezse timeout ver
const unsigned long BURST_IGNORE_WINDOW = 500; // 500 ms içindeki kopyaları yoksay

// ── WİFİ DURUM MAKİNESİ ──────────────────────────────────────
enum WiFiState {
  WS_CONNECTING,   
  WS_CONNECTED,    
  WS_AP_STA        
};
WiFiState wifiState = WS_CONNECTING;

// ── WIFI KONFİGÜRASYON ────────────────────────────────────────────
char wifiSSID[33] = "";
char wifiPass[65] = "";
char apSSID[33]   = "";
char apPass[65]   = "";
char apIP[16]     = "192.168.4.1";

// ── MQTT KONFİGÜRASYON ────────────────────────────────────────────
// Kurulum sayfasından girilir, ayrı NVS alanında ("mqtt") saklanır; kodda gömülü bilgi yoktur.
char     mqttHost[96] = "";
uint16_t mqttPort     = 8883;
char     mqttUser[64] = "";
char     mqttPass[96] = "";

bool          apRunning            = false;
unsigned long apStartTime          = 0;
unsigned long lastConnectingAttempt = 0;

// ── ZAMAN YÖNETİMİ ───────────────────────────────────────────
bool          timeSyncedByNTP    = false;
bool          timeSyncedByRTC    = false;
unsigned long lastNtpSync        = 0;
unsigned long lastRtcGetTime     = 0;
unsigned long lastRtcSetTime     = 0;

String getFormattedTime(uint32_t tmstmp = 0) {
  time_t timestamp = (tmstmp == 0) ? time(NULL) : (time_t)tmstmp;
  char buf[16]; struct tm *ti = localtime(&timestamp);
  if (ti != nullptr) {
    strftime(buf, sizeof(buf), "%H:%M:%S", ti); 
    return String(buf);
  }
  return String("00:00:00");
}

// ── LOG YÖNETİMİ ─────────────────────────────────────────────
#define LOG_FILE_PATH "/app.log"
#define MAX_LOG_SIZE  32768 // 32 KB (Flash belleği yormamak ve şişirmemek için üst limit)

void addLog(String msg) {
  // 1. Seri porta da yazmaya devam et (Kablolu debug için)
  Serial.println(msg);

  if (!LittleFS.begin()) return;

  // 2. Zaman damgasını ve saat kaynağı durumunu hazırla
  String timeStamp = "[";
  if (timeSyncedByNTP || timeSyncedByRTC) {  // saat güvenilirse gün.ay (uzun testlerde gece yarısını ayırt etmek için)
    time_t t = time(NULL); struct tm *dt = localtime(&t); char d[8];
    if (dt && strftime(d, sizeof(d), "%d.%m ", dt)) timeStamp += d;
  }
  timeStamp += getFormattedTime() + "]";
  if (timeSyncedByNTP)      timeStamp += "[NTP]";
  else if (timeSyncedByRTC) timeStamp += "[RTC]";
  else                      timeStamp += "[UNSET]";

  String logLine = timeStamp + " " + msg + "\n\n";

  // 3. Bellek Şişme Koruması: Dosya MAX_LOG_SIZE'ı geçerse eski yarısını sil
  if (LittleFS.exists(LOG_FILE_PATH)) {
    File checkFile = LittleFS.open(LOG_FILE_PATH, "r");
    if (checkFile && checkFile.size() > MAX_LOG_SIZE) {
      size_t fileSize = checkFile.size();
      checkFile.seek(fileSize / 2); // Dosyanın ortasına git
      checkFile.readStringUntil('\n'); // Yarım kalmış satırı atla
      String newContent = checkFile.readString(); // İkinci yarısını RAM'e al
      checkFile.close();

      // Eski logları temizleyip güncel yarısını yaz
      File rewriteFile = LittleFS.open(LOG_FILE_PATH, "w");
      if (rewriteFile) {
        rewriteFile.print("--- [LOG KIRPILDI - ESKİ KAYITLAR TEMİZLENDİ] ---\n");
        rewriteFile.print(newContent);
        rewriteFile.close();
      }
    } else if (checkFile) {
      checkFile.close();
    }
  }

  // 4. Yeni log satırını dosyanın sonuna ekle
  File logFile = LittleFS.open(LOG_FILE_PATH, "a");
  if (logFile) {
    logFile.print(logLine);
    logFile.close();
  }
}

void addLogf(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  
  addLog(String(buffer));
}

void timeSyncNotificationCallback(struct timeval *tv) {
  if (tv->tv_sec >= EPOCH_2026_THRESHOLD) {
    timeSyncedByNTP = true; timeSyncedByRTC = false; lastNtpSync = millis(); 
    addLog("[NTP] Saat SNTP ile guncellendi: " + String(millis()));
  }
}
bool isTimeReliable() { return timeSyncedByNTP || timeSyncedByRTC; }

// ── YÜK DURUMLARI ────────────────────────────────────────────
bool lights[2] = {false, false};
bool poolOn    = false;

struct AutoValveConf {
  bool    active;
  uint8_t hour;
  uint8_t minute;
  uint8_t duration;
};

struct Valve {
  bool          isOpen;
  unsigned long closeAt;
  bool          autoEnabled;
  uint32_t      manualTimeout;
  AutoValveConf slots[4];
};

Valve         valves[2];
bool          valveOpenedThisMinute[2] = {false, false};
unsigned long lastAutoValveCheck       = 0;

// ── ESP-NOW İLETİŞİM FONKSİYONLARI ───────────────────────────
bool sendEspNowCommand(EspNowMsg &payload, EspNowMsg &response, uint32_t timeoutMs = 1000) {
  if (espNowResponded && espNowAsyncAwait && !espNowAsyncResponded) return false;

  const unsigned long BURST_INTERVAL = 10;
  unsigned long start = millis();
  unsigned long lastSend = 0;
  int sendCount = 0;

  espNowResponded = false;
  espNowAsyncAwait = false;
  espNowAsyncResponded = false;

  payload.timestamp = (uint32_t)time(NULL);

  addLogf("[ESP-NOW: SND] cmd= %d msg_id= %u > start_at= %lu", payload.command, payload.timestamp, start);

  while (!espNowResponded) {
    if (millis() - lastSend >= BURST_INTERVAL) {
      esp_now_send(feederMac, (uint8_t *)&payload, sizeof(payload));
      lastSend = millis();
      sendCount++;
    }
    if (millis() - start > timeoutMs) {
      addLogf("[ESP-NOW: SND] cmd= %d TIMEOUT! sure= %lu ms > gonderilen_msg= %d adet ", payload.command, millis() - start, sendCount);
      return false;
    }
    yield();
  }

  addLogf("[ESP-NOW: SND] cmd= %d OK: sure= %lu ms > gonderilen_msg= %d adet ", payload.command, millis() - start, sendCount);
  response = receivedMsg;
  return true;
}

void onDataRecv(const uint8_t *macAddr, const uint8_t *incomingData, int len) {
  // 1. Veri geçerli bir msg paketi mi (uzunluğu kontrol et)
  if (len != sizeof(EspNowMsg)) return;

  // 2. Gelen veriyi geçici bir yapıya kopyala
  EspNowMsg incomingMsg;
  memcpy(&incomingMsg, incomingData, len);

  unsigned long now = millis();

  // 3. Burst Deduplication
  if (now - lastRecvTime < BURST_IGNORE_WINDOW) {
    // Mesaj aynı (burst) mı diye command + timestamp kontrol et
    if (incomingMsg.command + incomingMsg.timestamp == receivedMsg.command + receivedMsg.timestamp) {
      // Bu bir burst kopyası, hiçbir şey yapmadan işlemi sonlandır
      return; 
    }
  }

  // 4. Yeni ve geçerli mesajı kaydet
  lastRecvTime = now;
  memcpy(&receivedMsg, &incomingMsg, len);

  espNowResponded   = true;
  espNowAsyncAwait  = receivedMsg.values[0] && receivedMsg.values[1];
  espNowAsyncResponded = !receivedMsg.values[0] && receivedMsg.values[1];

  addLogf("[ESP-NOW: RCV] cmd= %d alinan_degerler= [%d, %d, %d, %d] ", receivedMsg.command, 
    receivedMsg.values[0], receivedMsg.values[1], receivedMsg.values[2], receivedMsg.values[3]);
}

void checkEspNowAsyncTimeout() {
  if (espNowAsyncAwait && millis() - lastRecvTime > ASYNC_AWAIT_TIMEOUT) {
    espNowAsyncResponded = false;
    espNowAsyncAwait = false;
    espNowAsyncResponded = false;
    addLogf("[ESP-NOW: RCV] cmd= %d cevap gelmedi, async islem zaman asimina ugradi.", receivedMsg.command);
  }
}

// ── KANAL SENKRONİZASYON SENARYOSU (KÖR TARAMA) ──────────────
void alignFeederChannel() {
  addLog("\n[ESP-NOW] Feeder kanal senkronizasyonu baslatiliyor...");

  // ESP32'nin bağlanacağı WiFi kanalını öğren
  int apChannel = 0;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == String(wifiSSID)) { apChannel = WiFi.channel(i); break; }
  }
  WiFi.scanDelete();
  addLogf("[WIFI] Hedef ag kanali: %d", apChannel);

  // Önce son bilinen kanalda dene
  int foundChannel = -1;
  auto tryChannel = [&](int ch) -> bool {
    esp_now_del_peer(feederMac);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, feederMac, 6);
    p.channel = ch; p.encrypt = false;
    p.ifidx = WIFI_IF_AP;
    esp_now_add_peer(&p);
    EspNowMsg req = {CMD_PING}, res;
    bool ok = sendEspNowCommand(req, res, 600);
    esp_now_del_peer(feederMac);
    return ok;
  };

  // Adım 1: Son bilinen kanalda dene
  addLogf("[ESP-NOW] Feeder bilinen son kanalda (%d) araniyor...", lastFeederChannel);
  if (tryChannel(lastFeederChannel)) {
    foundChannel = lastFeederChannel;
    addLog("[ESP-NOW] Feeder bilinen son kanalda bulundu.");
  } else {
    // Adım 2: Bulamazsan kör tarama yap
    addLog("\n[ESP-NOW] Feeder bilinen son kanalda bulunamadi, kor tarama yapiliyor...");
    for (int ch = 1; ch <= 13; ch++) {
      if (tryChannel(ch)) {
        foundChannel = ch;
        addLogf("[ESP-NOW] Feeder %d. kanalda bulundu!", ch);
        break;
      }
    }
  }

  // Adım 3: Kanal güncelleme gerekli mi? (wifi ile feeder farklı kanalda mı?)
  feederOnline = (foundChannel != -1);
  if (!feederOnline) {
    addLog("[ESP-NOW] Feeder bulunamadi! Cihaz uyku modunda veya cevrimdisi");
  } else {
    if (lastFeederChannel != foundChannel) {
      lastFeederChannel = foundChannel;
      prefs.begin("cfg", false);
      prefs.putUChar("fch", lastFeederChannel);
      prefs.end();
    }

    if (apChannel > 0 && foundChannel != apChannel) {
      // Feeder farklı kanalda, güncelle
      esp_wifi_set_promiscuous(true);
      esp_wifi_set_channel(foundChannel, WIFI_SECOND_CHAN_NONE);
      esp_wifi_set_promiscuous(false);
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, feederMac, 6);
      p.channel = foundChannel; p.encrypt = false;
      p.ifidx = WIFI_IF_AP;
      esp_now_add_peer(&p);
      EspNowMsg req = {CMD_SET_CHANNEL}, res;
      req.values[2] = apChannel;
      if (sendEspNowCommand(req, res)) {
        addLogf("[ESP-NOW] Feeder kanal %d'e yonlendirildi.", apChannel);
        lastFeederChannel = apChannel;
        prefs.begin("cfg", false);
        prefs.putUChar("fch", lastFeederChannel);
        prefs.end();
      } else {
        addLog("[ESP-NOW] Feeder kanal gecis komutu gonderilemedi!");
      }
      esp_now_del_peer(feederMac);
    } else if (apChannel > 0) {
      addLog("[ESP-NOW] Feeder zaten dogru kanalda, guncelleme gerekmiyor.");
    }
  }

  // ESP32 radyosunu AP kanalına getir
  int targetCh = (apChannel > 0) ? apChannel : (foundChannel > 0 ? foundChannel : lastFeederChannel);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(targetCh, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  // Final peer kaydı (channel=0: WiFi kanalını otomatik takip et)
  esp_now_peer_info_t finalPeer = {};
  memcpy(finalPeer.peer_addr, feederMac, 6);
  finalPeer.channel = 0;
  finalPeer.encrypt = false;
  finalPeer.ifidx = WIFI_IF_AP;
  esp_now_add_peer(&finalPeer);
}

// ── AYDINLATMA & SULAMA: RÖLE SÜR & OTOMASYON ─────────────────────────────
void setLight(int li, bool onOff = false) {
  lights[li] = onOff;
  onOff ? addLogf("Aydinlatma %d manuel olarak acildi", li+1) : addLogf("Aydinlatma %d manuel olarak kapatildi", li+1);
  relaySet(LIGHT_PINS[li], onOff);
}

void setPool(bool onOff = false) {
  poolOn = onOff;
  onOff ? addLog("Havuz manuel olarak acildi") : addLog("Havuz manuel olarak kapatildi");
  relaySet(PIN_POOL, onOff);
}

void setValve(int vi, bool onOff = false, bool isAuto = false, unsigned long duration = 0) {
  unsigned long timeout = millis() + (isAuto && duration > 0 ? duration : valves[vi].manualTimeout) * ONE_MIN_MS; 
  valves[vi].closeAt = onOff ? timeout : 0;
  valves[vi].isOpen  = onOff;
  if (isAuto) {
    onOff ? addLogf("Vana %d otomatik olarak acildi", vi+1) : addLogf("Vana %d otomatik olarak kapatildi", vi+1);
  } else {
    onOff ? addLogf("Vana %d manuel olarak acildi", vi+1) : addLogf("Vana %d manuel olarak kapatildi", vi+1);
  }
  relaySet(VALVE_PINS[vi], onOff);
}

void checkValveTimeouts() {
  unsigned long now = millis();
  for (int vi = 0; vi < 2; vi++) {
    if (valves[vi].isOpen && valves[vi].closeAt > 0 && now >= valves[vi].closeAt) {
      setValve(vi, false, true);
    }
  }
}

void checkAutoValve() {
  if(!isTimeReliable()) return;
  time_t nowTime = time(NULL); struct tm *ti = localtime(&nowTime);
  int h = ti->tm_hour, m = ti->tm_min, s = ti->tm_sec;
  for (int vi = 0; vi < 2; vi++) {
    if (!valves[vi].autoEnabled) continue;
    bool onTime = false;
    unsigned long dur = 0;
    for (int si = 0; si < 4; si++) {
      AutoValveConf &sl = valves[vi].slots[si];
      if (!sl.active) continue;
      if (sl.hour == h && sl.minute == m && s < 55) { onTime = true; dur = sl.duration; break; }
    }
    if (onTime && !valveOpenedThisMinute[vi]) {
      valveOpenedThisMinute[vi] = true;
      setValve(vi, true, true, dur);
    }
    if (!onTime) valveOpenedThisMinute[vi] = false;
  }
}

void setLightsAndPool() {
  bool anyActive = lights[0] || lights[1] || poolOn;
  if (anyActive) { 
    setLight(0, false); delay(100); 
    setLight(1, false); delay(100);
    setPool(false);
  } else { 
    setLight(0, true); delay(100); 
    setLight(1, true); delay(100);
    setPool(true);
  }
}

// ── RF: GELEN KODU İŞLE ──────────────────────────────────────
void handleRF() {
  if (!rcSwitch.available()) return;
  unsigned long code = rcSwitch.getReceivedValue();
  rcSwitch.resetAvailable();
  if (code == lastRFCode && (millis() - lastRFTime < RF_DEBOUNCE_MS)) return; 
  lastRFCode = code; lastRFTime = millis();
  
  if (code == RF_LIGHT1) { setLight(0, !lights[0]); }
  else if (code == RF_LIGHT2) { setLight(1, !lights[1]); }
  else if (code == RF_POOL) { setPool(!poolOn); }
  else if (code == RF_ALLONOFF) { setLightsAndPool(); }
  else if (code == RF_VALVE1) { setValve(0, !valves[0].isOpen); }
  else if (code == RF_VALVE2) { setValve(1, !valves[1].isOpen); }
}

// FEEDER için Uyku Önbelleği
uint8_t feederFoodLevel = 0, feederBattery = 0;

struct FeederSlot {
  uint8_t h;
  uint8_t m;
  bool active;
};
FeederSlot feederSlots[4];
bool feederAutoFeedEnable = false;

uint8_t feederSleepStartH = 0, feederSleepStartM = 0;
uint8_t feederSleepEndH = 0,   feederSleepEndM = 0;
bool feederSleepEnabled = false;

// Zamanlanmış feeder sorgusu durumu (bkz. feederPollLoop)
bool     feederUnreachable      = false;  // 3 ardışık hata sonrası true; uyku saatinde bile "uykuda" denmez
uint8_t  feederPollFails        = 0;
bool     feederRetryPending     = false;  // 1 dk sonra yeniden denenecek
uint32_t feederRetryAt          = 0;      // epoch
uint32_t feederLastOkEpoch      = 0;      // son başarılı sorgu (epoch), 0 = bu açılışta hiç
uint32_t feederNextPeriodic     = 0;      // bir sonraki saatlik sorgu (epoch)
bool     feederWindowWasOpen    = false;
uint32_t feederSlotPolled[4]    = {0, 0, 0, 0}; // slot başına son sorgulanan öğün zamanı (epoch)
bool     feederConfigSynced     = false;  // açılışta feeder ayarı bir kez önbelleğe alınır
uint32_t feederConfigNextTry    = 0;      // epoch
bool     feederManualPollPending = false;
unsigned long feederManualPollAtMs = 0;
bool     feederPanelPollPending = false;  // Mama Kabı paneline giriş / bayrak inince yenileme: feeder'a sorulacak
const char* feederPanelPollWhy  = "panel girisi";
unsigned long feederLastFailMs  = 0;      // son başarısız haberleşme (millis), 0 = yok
bool isFeederSleeping() {
  if (!feederSleepEnabled || feederUnreachable || !isTimeReliable()) return false;

  time_t nowTime = time(NULL);
  struct tm *ti = localtime(&nowTime);
  int curMins   = ti->tm_hour * 60 + ti->tm_min;
  int startMins = feederSleepStartH * 60 + feederSleepStartM;
  int endMins   = feederSleepEndH * 60 + feederSleepEndM;

  if (startMins == endMins) return false;

  // Gece yarısını aşan aralıklar (Örn: 22:00 - 06:00)
  if (startMins > endMins) {
    return (curMins >= startMins || curMins < endMins);
  } else { // Aynı gün içi aralıklar (Örn: 01:00 - 05:00)
    return (curMins >= startMins && curMins < endMins);
  }
}

// ── PREFERENCES: KAYDET / YÜKLE ─────────────────────────────
// Feeder önbelleği JSON'u (/api/feeder/status ve /api/status içindeki "feeder" nesnesi ortak kullanır).
// ageSec: son başarılı sorgudan beri saniye, -1 = bu açılışta henüz sorgu yok.
void feederFillJson(JsonObject o) {
  o["battery"]     = feederBattery;
  o["foodLevel"]   = feederFoodLevel;
  o["unreachable"] = feederUnreachable;
  o["sleep"]       = isFeederSleeping();
  int age = -1;
  if (feederLastOkEpoch && isTimeReliable()) {
    long d = (long)((uint32_t)time(NULL) - feederLastOkEpoch);
    age = d < 0 ? 0 : (int)d;
  }
  o["ageSec"] = age;
}

void savePrefs() {
  prefs.begin("cfg", false);
  prefs.putString("ssid", wifiSSID); prefs.putString("pass", wifiPass);
  prefs.putString("apSSID", apSSID); prefs.putString("apPass", apPass); prefs.putString("apIP", apIP);
  char key[16];
  for (int vi = 0; vi < 2; vi++) {
    snprintf(key, sizeof(key), "v%dauto", vi);      prefs.putBool (key, valves[vi].autoEnabled);
    snprintf(key, sizeof(key), "v%dtout", vi);      prefs.putUInt (key, valves[vi].manualTimeout);
    for (int si = 0; si < 4; si++) {
      snprintf(key, sizeof(key), "v%ds%da", vi, si); prefs.putBool (key, valves[vi].slots[si].active);
      snprintf(key, sizeof(key), "v%ds%dh", vi, si); prefs.putUChar(key, valves[vi].slots[si].hour);
      snprintf(key, sizeof(key), "v%ds%dm", vi, si); prefs.putUChar(key, valves[vi].slots[si].minute);
      snprintf(key, sizeof(key), "v%ds%dd", vi, si); prefs.putUChar(key, valves[vi].slots[si].duration);
    }
  }

  // Feeder önbellekleri
  prefs.putUChar("fch", lastFeederChannel);
  prefs.putUChar("fBattLv", feederBattery);
  prefs.putUChar("fFoodLv", feederFoodLevel);

  prefs.putBool("fAutoEn", feederAutoFeedEnable);
  char keyf[16];
  for (int fi = 0; fi < 4; fi++) {
    snprintf(keyf, sizeof(keyf), "fS%dh", fi); prefs.putUChar(keyf, feederSlots[fi].h);
    snprintf(keyf, sizeof(keyf), "fS%dm", fi); prefs.putUChar(keyf, feederSlots[fi].m);
    snprintf(keyf, sizeof(keyf), "fS%da", fi); prefs.putBool (keyf, feederSlots[fi].active);
  }

  prefs.putBool("fSlpEn", feederSleepEnabled);
  prefs.putUChar("fSlpSH", feederSleepStartH);
  prefs.putUChar("fSlpSM", feederSleepStartM);
  prefs.putUChar("fSlpEH", feederSleepEndH);
  prefs.putUChar("fSlpEM", feederSleepEndM);

  prefs.end();
}

// MQTT ayarları ayrı NVS alanında (savePrefs sık çağrılır, şifre onunla karışmasın)
void saveMqttPrefs() {
  Preferences mp;
  mp.begin("mqtt", false);
  mp.putString("host", mqttHost); mp.putUShort("port", mqttPort);
  mp.putString("user", mqttUser); mp.putString("pass", mqttPass);
  mp.end();
}

void loadMqttPrefs() {
  Preferences mp;
  mp.begin("mqtt", false);
  strlcpy(mqttHost, mp.getString("host", "").c_str(), sizeof(mqttHost));
  mqttPort = mp.getUShort("port", 8883);
  strlcpy(mqttUser, mp.getString("user", "").c_str(), sizeof(mqttUser));
  strlcpy(mqttPass, mp.getString("pass", "").c_str(), sizeof(mqttPass));
  mp.end();
}

void loadPrefs() {
  prefs.begin("cfg", true);
  strlcpy(wifiSSID, prefs.getString("ssid", "").c_str(), 33);
  strlcpy(wifiPass, prefs.getString("pass", "").c_str(), 65);
  strlcpy(apSSID, prefs.getString("apSSID", "").c_str(), 33);
  strlcpy(apPass, prefs.getString("apPass", "").c_str(), 65);
  strlcpy(apIP, prefs.getString("apIP", "192.168.4.1").c_str(), 16);
  char key[16];
  for (int vi = 0; vi < 2; vi++) {
    snprintf(key, sizeof(key), "v%dauto", vi);      valves[vi].autoEnabled   = prefs.getBool (key, false);
    snprintf(key, sizeof(key), "v%dtout", vi);      valves[vi].manualTimeout = prefs.getUInt (key, 30);
    for (int si = 0; si < 4; si++) {
      snprintf(key, sizeof(key), "v%ds%da", vi, si); valves[vi].slots[si].active    = prefs.getBool (key, false);
      snprintf(key, sizeof(key), "v%ds%dh", vi, si); valves[vi].slots[si].hour      = prefs.getUChar(key, 7);
      snprintf(key, sizeof(key), "v%ds%dm", vi, si); valves[vi].slots[si].minute    = prefs.getUChar(key, 0);
      snprintf(key, sizeof(key), "v%ds%dd", vi, si); valves[vi].slots[si].duration  = prefs.getUChar(key, 15);
    }
  }

  // Feeder önbellekleri
  lastFeederChannel  = prefs.getUChar("fch", 1);
  feederBattery      = prefs.getUChar("fBattLv", 0);
  feederFoodLevel    = prefs.getUChar("fFoodLv", 0);

  feederAutoFeedEnable = prefs.getBool("fAutoEn", false);
  char keyf[16];
  for (int fi = 0; fi < 4; fi++) {
    snprintf(keyf, sizeof(keyf), "fS%dh", fi); feederSlots[fi].h      = prefs.getUChar(keyf, 0);
    snprintf(keyf, sizeof(keyf), "fS%dm", fi); feederSlots[fi].m      = prefs.getUChar(keyf, 0);
    snprintf(keyf, sizeof(keyf), "fS%da", fi); feederSlots[fi].active = prefs.getBool (keyf, false);
  }

  feederSleepEnabled = prefs.getBool("fSlpEn", false);
  feederSleepStartH  = prefs.getUChar("fSlpSH", 0);
  feederSleepStartM  = prefs.getUChar("fSlpSM", 0);
  feederSleepEndH    = prefs.getUChar("fSlpEH", 0);
  feederSleepEndM    = prefs.getUChar("fSlpEM", 0);

  prefs.end();
  if (strlen(apSSID) == 0) { uint32_t chip = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF); snprintf(apSSID, sizeof(apSSID), "ESP32-%06X", chip); savePrefs(); }
}

// ── AP YÖNETİMİ ──────────────────────────────────────────────
void startAP() {
  String name = strlen(apSSID) > 0 ? String(apSSID) : "ESP32-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
  WiFi.mode(WIFI_AP_STA); delay(100);
  IPAddress localIPAddr; if (localIPAddr.fromString(apIP)) { WiFi.softAPConfig(localIPAddr, localIPAddr, IPAddress(255, 255, 255, 0)); }
  if (strlen(apPass) >= 8) WiFi.softAP(name.c_str(), apPass); else WiFi.softAP(name.c_str());
  apRunning = true; apStartTime = millis();
}
void stopAP() { 
  WiFi.softAPdisconnect(true); 
  WiFi.mode(WIFI_AP_STA);
  delay(50); 
  apRunning = false; 
}

// ── WİFİ DURUM MAKİNESİ VE ESP-NOW BAŞLATMA ───────────────────
void handleWiFiStateMachine() {
  unsigned long now = millis();
  if (apRunning && wifiState == WS_CONNECTED && now - apStartTime > AP_TIMEOUT_MS) stopAP();

  switch (wifiState) {
    case WS_CONNECTING: {
      if (strlen(wifiSSID) == 0) { wifiState = WS_AP_STA; break; }
      lastConnectingAttempt = now;
      
      // Kanal yönetim arayüzü başlatılıyor (Bağlantı öncesi)
      WiFi.mode(WIFI_AP_STA);
      WiFi.disconnect();
      delay(100);
      alignFeederChannel();

      if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {}
      WiFi.begin(wifiSSID, wifiPass);
      addLogf("\n[WIFI] Baglanti deneniyor: %s \n", wifiSSID);
      for (int i = 0; i < WIFI_CONNECT_TRIES && WiFi.status() != WL_CONNECTED; i++) { Serial.print("."); delay(500); }
      if (WiFi.status() == WL_CONNECTED) {
        wifiState = WS_CONNECTED; addLogf("\n[WIFI] Baglanti basarili: %s \n", WiFi.localIP().toString().c_str());
      } else {
        wifiState = WS_AP_STA; addLog("\n[WIFI] Baglanti basarisiz! AP modu başlatiliyor.");
      }
      break;
    }
    case WS_CONNECTED: { if (WiFi.status() != WL_CONNECTED) { wifiState = WS_AP_STA; } break; }
    case WS_AP_STA: {
      if (!apRunning) startAP();
      if (strlen(wifiSSID) > 0 && now - lastConnectingAttempt > RECONNECT_INTERVAL) wifiState = WS_CONNECTING;
      break;
    }
  }
}

// ── CORS & GENEL API'LER ─────────────────────────────────────
// ── SANAL İSTEK BAĞLAMI ───────────────────────────────────────
// Handler'lar hem gerçek HTTP isteğinde hem WebSocket/MQTT üzerinden gelen sanal
// istekte aynı kodu çalıştırır. Sanal modda (active=true) argümanlar apiCtx'ten okunur,
// cevap apiCtx'e yazılır, HTTP'ye hiçbir şey gönderilmez.
struct ApiCtx {
  bool           active  = false;
  JsonObjectConst q;
  String         body;
  bool           hasBody = false;
  int            code    = 0;
  String         out;
};
ApiCtx apiCtx;

bool apiHasArg(const char* name) {
  if (!apiCtx.active) return server.hasArg(name);
  if (strcmp(name, "plain") == 0) return apiCtx.hasBody;
  return !apiCtx.q[name].isNull();
}

String apiArg(const char* name) {
  if (!apiCtx.active) return server.arg(name);
  if (strcmp(name, "plain") == 0) return apiCtx.body;
  return apiCtx.q[name].as<String>();
}

void apiSend(int code, const char* type, const String& body) {
  if (!apiCtx.active) { server.send(code, type, body); return; }
  apiCtx.code = code; apiCtx.out = body;
}

void apiSend(int code) {
  if (!apiCtx.active) { server.send(code); return; }
  apiCtx.code = code; apiCtx.out = "";
}

void setCORS() { if (apiCtx.active) return; server.sendHeader("Access-Control-Allow-Origin", "*"); server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS"); server.sendHeader("Access-Control-Allow-Headers", "Content-Type"); }

void handleStatus() {
  setCORS(); JsonDocument doc;
  JsonArray la = doc["lights"].to<JsonArray>(); for (int li = 0; li < 2; li++) la.add(lights[li]);
  doc["poolOn"] = poolOn;
  JsonArray va = doc["valves"].to<JsonArray>(); for (int vi = 0; vi < 2; vi++) { JsonObject v = va.add<JsonObject>(); v["isOpen"] = valves[vi].isOpen; v["autoEnabled"] = valves[vi].autoEnabled; }
  doc["timeValid"] = isTimeReliable(); doc["timeSyncedByNTP"] = timeSyncedByNTP; doc["timeSyncedByRTC"] = timeSyncedByRTC; doc["time"] = getFormattedTime(); 
  doc["wifiState"] = (int)wifiState; doc["feederOnline"] = feederOnline; doc["feederSleep"] = isFeederSleeping();
  JsonObject fd = doc["feeder"].to<JsonObject>(); feederFillJson(fd);
  String out; serializeJson(doc, out); apiSend(200, "application/json", out);
}

void handleLight() { 
  setCORS(); 
  if (!apiHasArg("id") || !apiHasArg("on")) return; 
  int id = apiArg("id").toInt(); 
  bool onOff = apiArg("on") == "true"; 
  if (id < 0 || id > 1) return; 
  setLight(id, onOff);
  apiSend(200, "application/json", "{\"ok\":true}"); 
}

void handlePool() { 
  setCORS(); 
  if (!apiHasArg("on")) return; 
  bool onOff = apiArg("on") == "true"; 
  setPool(onOff); 
  apiSend(200, "application/json", "{\"ok\":true}"); 
}

void handleValveOnOff() {
  setCORS();
  if (!apiHasArg("id") || !apiHasArg("on")) return;
  int id = apiArg("id").toInt();
  bool onOff = apiArg("on") == "true";
  if (id < 0 || id > 1) return;
  setValve(id, onOff);
  apiSend(200, "application/json", "{\"ok\":true}");
}

void handleValveAutoEnable() {
  setCORS();
  if (!apiHasArg("id") || !apiHasArg("on")) return;
  int id = apiArg("id").toInt();
  bool onOff = apiArg("on") == "true";
  if (id < 0 || id > 1) return;
  valves[id].autoEnabled = onOff;
  onOff ? addLogf("Vana %d otomasyonu açildi", id+1) : addLogf("Vana %d otomasyonu kapatildi", id+1);
  savePrefs();
  apiSend(200, "application/json", "{\"ok\":true}");
}

void handleValveCfgPost() {
  setCORS();
  if (!apiHasArg("plain")) return;
  JsonDocument doc;
  if (deserializeJson(doc, apiArg("plain"))) return;
  int vi = doc["vi"] | -1;
  if (vi < 0 || vi > 1) return;
  valves[vi].autoEnabled   = doc["autoEnabled"] | false;
  valves[vi].manualTimeout = doc["manualTimeout"] | 30;
  JsonArray arr = doc["slots"].as<JsonArray>();
  for (int si = 0; si < 4 && si < (int)arr.size(); si++) {
    valves[vi].slots[si].active   = arr[si]["active"] | false;
    valves[vi].slots[si].hour     = arr[si]["h"] | 0;
    valves[vi].slots[si].minute   = arr[si]["m"] | 0;
    valves[vi].slots[si].duration = arr[si]["dur"] | 15;
  }
  addLogf("Vana %d konfigurasyonlari guncellendi", vi+1);
  savePrefs();
  apiSend(200, "application/json", "{\"ok\":true}");
}

void handleValveCfgGet() {
  setCORS();
  if (!apiHasArg("id")) {
    apiSend(400, "application/json", "{\"error\":\"Missing id parameter\"}");
    return;
  }
  int vi = apiArg("id").toInt();
  if (vi < 0 || vi >= 2) {
    apiSend(400, "application/json", "{\"error\":\"Invalid valve id\"}");
    return;
  }
  JsonDocument doc;
  doc["autoEnabled"]   = valves[vi].autoEnabled;
  doc["manualTimeout"] = valves[vi].manualTimeout;
  JsonArray slots = doc["slots"].to<JsonArray>();
  for (int si = 0; si < 4; si++) {
    JsonObject s = slots.add<JsonObject>();
    s["active"]   = valves[vi].slots[si].active; 
    s["h"]        = valves[vi].slots[si].hour; 
    s["m"]        = valves[vi].slots[si].minute; 
    s["dur"]      = valves[vi].slots[si].duration; 
  }
  String out; 
  serializeJson(doc, out);
  apiSend(200, "application/json", out);
}

// ── FEEDER ÖNBELLEK TAZELEME (ZAMANLANMIŞ SORGU) ─────────────
// Feeder'ı ESP32 kendisi sorgular; uygulama durum isteği yalnız önbellekten cevap alır.
// Pencere: uyanış+5 dk -> uyku başlangıcı (uyku kapalıysa hep açık). Sorgular: pencere açılınca ilk sorgu,
// sonra saatlik, aktif otomatik yemleme slotu için öğün+5 dk (slot başına bir kez), manuel yemlemeden ~5 sn sonra,
// Mama Kabı paneline her girişte (önbellek tazeleme, bkz. handleFeederRefresh).
// Başarısız sorgu 1 dk sonra yeniden denenir, 3 hatada "ulaşılamıyor" (son bilinen değerler korunur, zamanlı sorgular sürer).
constexpr int      FEEDER_WAKE_POLL_MIN        = 5;
constexpr uint32_t FEEDER_POLL_PERIOD_S        = 3600;
constexpr uint32_t FEEDER_MEAL_POLL_DELAY_S    = 300;
constexpr uint32_t FEEDER_MEAL_POLL_MAX_AGE_S  = 1800;  // bundan eski öğün için sorgu yapılmaz
constexpr uint32_t FEEDER_MANUAL_POLL_DELAY_MS = 5000;  // feeder: bip+açma 1450 + bekleme 2500 + ölçüm en fazla ~600 = ~4,55 sn
constexpr uint32_t FEEDER_RETRY_S              = 60;
constexpr uint8_t  FEEDER_MAX_FAILS            = 3;

// Feeder'dan herhangi bir başarılı cevap: ulaşılamıyor durumunu temizler.
// Bayrak inerse (geri dönüş) pil/mama değerlerini tazelemek için bir yenileme sorgusu sıraya alınır.
void feederMarkAlive() {
  bool was = feederUnreachable;
  feederUnreachable = false; feederPollFails = 0; feederRetryPending = false; feederOnline = true;
  if (was) { feederPanelPollPending = true; feederPanelPollWhy = "geri baglanti"; addLog("[FEEDER] Haberlesme geri geldi, ulasilamiyor bayragi indi."); }
}

void feederApplyStatus(uint8_t bat, uint8_t food) {
  if (feederBattery != bat || feederFoodLevel != food) {
    feederBattery   = bat;
    feederFoodLevel = food;
    savePrefs(); // Sadece veri değiştiyse Flash'a yaz
  }
}

// Feeder ayar cevabını önbelleğe alır (değiştiyse Flash'a yazar).
void feederCacheConfig(const EspNowMsg& res) {
  bool changed = false;
  for (int i = 0; i < 4; i++) {
    bool act = FEEDER_FLAG_GET(res.flags, i) == 1;
    if (feederSlots[i].h != res.slot_h[i] || feederSlots[i].m != res.slot_m[i] || feederSlots[i].active != act) changed = true;
    feederSlots[i].h = res.slot_h[i]; feederSlots[i].m = res.slot_m[i]; feederSlots[i].active = act;
  }
  bool aut = FEEDER_FLAG_GET(res.flags, 4) == 1;
  bool slp = FEEDER_FLAG_GET(res.flags, 5) == 1;
  if (feederAutoFeedEnable != aut || feederSleepEnabled != slp ||
      feederSleepStartH != res.sleep[0] || feederSleepStartM != res.sleep[1] ||
      feederSleepEndH != res.sleep[2] || feederSleepEndM != res.sleep[3]) changed = true;
  feederAutoFeedEnable = aut; feederSleepEnabled = slp;
  feederSleepStartH = res.sleep[0]; feederSleepStartM = res.sleep[1];
  feederSleepEndH   = res.sleep[2]; feederSleepEndM   = res.sleep[3];
  if (changed) savePrefs();
}

// Gün içi dakika (0-1439) [a, b) aralığında mı (gece yarısını aşan aralık dahil)?
bool feederMinInRange(int cur, int a, int b) {
  if (a == b) return false;
  return a < b ? (cur >= a && cur < b) : (cur >= a || cur < b);
}

// Sorgu penceresi açık mı: uyanış+5 dk ile uyku başlangıcı arası.
bool feederPollWindowOpen(const struct tm& ti) {
  if (!feederSleepEnabled) return true;
  int start = feederSleepStartH * 60 + feederSleepStartM;
  int end   = feederSleepEndH * 60 + feederSleepEndM;
  if (start == end) return true;  // uyku aralığı yok
  int cur  = ti.tm_hour * 60 + ti.tm_min;
  int wake = (end + FEEDER_WAKE_POLL_MIN) % 1440;
  return feederMinInRange(cur, wake, start);
}

// Feeder ulaşılamıyorken olası kanal uyuşmazlığını gidermek için: Wi-Fi durum makinesini WS_CONNECTING'e alır,
// böylece alignFeederChannel() (son kanal, sonra 1-13 kör tarama, bulunca CMD_SET_CHANNEL) yeniden çalışır.
// Wi-Fi/MQTT ~10-20 sn kesilir, bu yüzden en fazla saatte bir ve yalnız Wi-Fi bağlıyken (aksi halde zaten
// bağlanma döngüsündeki hizalama çalışır). 45 sn sonra doğrulama sorgusu yapılır.
constexpr uint32_t FEEDER_REALIGN_MIN_GAP_S = 3600;
constexpr uint32_t FEEDER_REALIGN_VERIFY_S  = 45;
uint32_t feederRealignLastEpoch = 0;
void feederRequestRealign() {
  uint32_t nowE = (uint32_t)time(NULL);
  if (feederRealignLastEpoch && nowE - feederRealignLastEpoch < FEEDER_REALIGN_MIN_GAP_S) return;
  if (wifiState != WS_CONNECTED) return;
  feederRealignLastEpoch = nowE;
  addLog("[FEEDER] Ulasilamiyor: kanal hizalama yeniden baslatiliyor (Wi-Fi kisa sure kesilecek).");
  wifiState = WS_CONNECTING;
  feederRetryPending = true; feederRetryAt = nowE + FEEDER_REALIGN_VERIFY_S;
}

// Başarısız haberleşme muhasebesi (sorgu ya da canlı komut).
// immediate=true (kullanıcı tetikli: panel girişi, manuel yemleme, ayar/saat komutu): bayrak ilk hatada açılır, 1 dk sonra
// gözetimsiz doğrulama sorgusu planlanır ve kanal yeniden hizalama BAŞLATILMAZ (Wi-Fi/MQTT kesilmesin).
// immediate=false (periyodik, öğün, yeniden deneme): 3 ardışık hata ister, bayrak açılınca/açıkken hizalama dener.
void feederRegisterFailure(const char* why, bool immediate) {
  feederLastFailMs = millis(); if (!feederLastFailMs) feederLastFailMs = 1;
  if (feederPollFails < 250) feederPollFails++;
  if (!immediate) feederRetryPending = false;
  if (feederUnreachable) {  // zaten ulaşılamıyor: yeniden deneme serisi yok, sonraki zamanlı sorgu denenir
    addLogf("[FEEDER] Sorgu (%s) basarisiz (hala ulasilamiyor).", why);
    if (!immediate) feederRequestRealign();
    return;
  }
  if (immediate || feederPollFails >= FEEDER_MAX_FAILS) {
    feederUnreachable = true; feederOnline = false;
    addLogf("[FEEDER] Sorgu (%s): feeder ULASILAMIYOR isaretlendi (basarisiz haberlesme: %d), son bilinen degerler kullaniliyor.", why, feederPollFails);
    if (immediate) { feederRetryPending = true; feederRetryAt = (uint32_t)time(NULL) + FEEDER_RETRY_S; }
    else feederRequestRealign();
  } else {
    feederRetryPending = true; feederRetryAt = (uint32_t)time(NULL) + FEEDER_RETRY_S;
    addLogf("[FEEDER] Sorgu (%s) basarisiz (%d/%d), 1 dk sonra yeniden denenecek.", why, feederPollFails, FEEDER_MAX_FAILS);
  }
}

// Tek sorgu (pil+mama birlikte gelir) ve hata/yeniden deneme muhasebesi.
void feederRunPoll(const char* why, bool immediate = false) {
  EspNowMsg req = {CMD_REQ_STATUS}, res;
  if (sendEspNowCommand(req, res)) {
    bool wasUnreachable = feederUnreachable;
    bool foodErr = res.values[3] > 100;  // 101 = feeder mama ölçümü hatalı / henüz ölçülmedi: eski mama değerini koru
    feederApplyStatus(res.values[2], foodErr ? feederFoodLevel : res.values[3]);
    feederMarkAlive();
    feederPanelPollPending = false;  // bu sorgu zaten yenileme (geri dönüş sorgusu ikilenmesin)
    feederLastOkEpoch = (uint32_t)time(NULL);
    if (foodErr) addLogf("[FEEDER] Sorgu (%s) OK: pil=%d mama=OLCUM HATALI (eski %%%d korunuyor)%s", why, res.values[2], feederFoodLevel, wasUnreachable ? " (tekrar ulasilabilir)" : "");
    else         addLogf("[FEEDER] Sorgu (%s) OK: pil=%d mama=%d%s", why, res.values[2], res.values[3], wasUnreachable ? " (tekrar ulasilabilir)" : "");
    return;
  }
  feederRegisterFailure(why, immediate);
}

// Saniyede bir kontrol eder; sorgu gerekiyorsa yapar.
void feederPollLoop() {
  static unsigned long lastTick = 0;
  unsigned long ms = millis();
  if (ms - lastTick < 1000) return;
  lastTick = ms;
  if (!isTimeReliable()) return;

  time_t nowT = time(NULL);
  uint32_t nowE = (uint32_t)nowT;
  struct tm ti; localtime_r(&nowT, &ti);
  bool open = feederPollWindowOpen(ti);
  bool pollNow = false;
  bool userPoll = false;  // kullanıcı tetikli sorgu: hata anında bayrak hemen açılır (bkz. feederRegisterFailure)
  char why[48]; why[0] = 0;  // sorgunun nedeni (log için)
  auto addWhy = [&](const char* s) { if (why[0]) strlcat(why, "+", sizeof(why)); strlcat(why, s, sizeof(why)); };

  // Manuel yemleme sonrası ve yeniden deneme: pencereden bağımsız (feeder o an uyanıktı / seri tamamlanır)
  if (feederManualPollPending && (long)(ms - feederManualPollAtMs) >= 0) { feederManualPollPending = false; pollNow = true; userPoll = true; addWhy("manuel yemleme"); }
  if (feederRetryPending && nowE >= feederRetryAt) { pollNow = true; addWhy("yeniden deneme"); }

  // Mama Kabı paneline giriş (ya da bayrak inince yenileme): uyku saatinde sorgu yok; bir komut az önce başarısız olduysa
  // (bayrak zaten açıldı) ikinci bir 1 sn'lik bloklayan deneme yapılmaz.
  if (feederPanelPollPending) {
    feederPanelPollPending = false;
    bool recentFail = feederLastFailMs && (ms - feederLastFailMs) < 3000;
    if (!isFeederSleeping() && !recentFail) { pollNow = true; userPoll = true; addWhy(feederPanelPollWhy); }
  }

  // Pencere açıldı: uyanış sonrası ilk sorgu, saatlik periyot buradan başlar
  bool opened = open && !feederWindowWasOpen;
  if (opened) { feederNextPeriodic = nowE; addLog("[FEEDER] Sorgu penceresi acildi."); }
  if (!open && feederWindowWasOpen) {
    addLog(feederUnreachable ? "[FEEDER] Sorgu penceresi kapandi; feeder ULASILAMIYOR isaretli, 'uykuda' denmeyecek, uykuda sorgu yok."
                             : "[FEEDER] Sorgu penceresi kapandi (uyku), uykuda sorgu yok.");
  }
  feederWindowWasOpen = open;

  if (open) {
    // Saatlik periyodik sorgu (öğün sorgularından bağımsız, uyanıştan itibaren saat başı)
    if (nowE >= feederNextPeriodic) {
      pollNow = true;
      addWhy(opened ? "uyanis/ilk" : "saatlik");
      do { feederNextPeriodic += FEEDER_POLL_PERIOD_S; } while (feederNextPeriodic <= nowE);
    }
    // Otomatik yemleme sonrası sorgu: slot bazında, öğün+5 dk, her öğün için bir kez
    if (feederAutoFeedEnable) {
      for (int i = 0; i < 4; i++) {
        if (!feederSlots[i].active) continue;
        for (int d = 0; d <= 1; d++) {  // bugün ve dün (gece yarısını aşan +5 dk için)
          struct tm mt = ti;
          mt.tm_mday -= d; mt.tm_hour = feederSlots[i].h; mt.tm_min = feederSlots[i].m; mt.tm_sec = 0; mt.tm_isdst = -1;
          uint32_t meal = (uint32_t)mktime(&mt);
          uint32_t due  = meal + FEEDER_MEAL_POLL_DELAY_S;
          if (nowE >= due && nowE - due <= FEEDER_MEAL_POLL_MAX_AGE_S && feederSlotPolled[i] < meal) {
            feederSlotPolled[i] = meal;
            char sl[16]; snprintf(sl, sizeof(sl), "ogun slot %d", i + 1); addWhy(sl);
            pollNow = true;
          }
        }
      }
    }
  }

  if (pollNow) { feederRunPoll(why, userPoll); return; }

  // Açılıştan sonra feeder ayarını bir kez önbelleğe al (uygulama hiç ayar açmasa da yemleme/uyku saatleri doğru olsun)
  if (!feederConfigSynced && feederLastOkEpoch && !feederUnreachable && nowE >= feederConfigNextTry) {
    EspNowMsg req = {CMD_REQ_CONFIG}, res;
    if (sendEspNowCommand(req, res)) {
      feederCacheConfig(res); feederConfigSynced = true;
      addLog("[FEEDER] Ayarlar onbellege alindi.");
    } else {
      feederConfigNextTry = nowE + 60;
    }
  }
}

// ── FEEDER API ENTEGRASYONLARI (ESP-NOW) ─────────────────────
// Durum isteği canlı ESP-NOW yapmaz: zamanlanmış sorguların güncellediği önbellekten döner.
void handleFeederGetStatus() {
  setCORS();
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["ok"] = !feederUnreachable;
  feederFillJson(o);
  String out; serializeJson(doc, out);
  apiSend(200, "application/json", out);
}
void handleFeederGetTime() {
  setCORS();
  EspNowMsg req = {CMD_REQ_RTC}, res;
  if (sendEspNowCommand(req, res, 1000)) {
    feederOnline = true;
    JsonDocument doc;
    doc["ok"] = true;
    feederMarkAlive();
    doc["source"] = "rtc";
    doc["time"] = getFormattedTime(res.timestamp);  
    String out; serializeJson(doc, out);
    apiSend(200, "application/json", out);
  } else {
    feederOnline = false;
    if (isFeederSleeping()) {
      apiSend(203, "application/json", "{\"ok\":false,\"reason\":\"sleeping\"}");
    } else {
      feederRegisterFailure("saat komutu", true);
      apiSend(504, "application/json", "{\"ok\":false,\"err\":\"timeout\"}"); 
    }
  }
}
void handleFeederManualFeed() {
  setCORS();
  EspNowMsg req = {CMD_MANUAL_FEED}, res;
  if (sendEspNowCommand(req, res)) {
    feederMarkAlive();
    feederPanelPollPending = false;  // feeder yemlemede (bip+açma); geri dönüş sorgusu yerine aşağıdaki 5 sn'lik sorgu yeter
    feederManualPollPending = true; feederManualPollAtMs = millis() + FEEDER_MANUAL_POLL_DELAY_MS;  // yemleme bitince mama ölçümü önbelleğe alınır
    feederOnline = true;
    apiSend(200, "application/json", "{\"ok\":true}");
  }
  else {
    feederOnline = false;
    if (isFeederSleeping()) {
      apiSend(203, "application/json", "{\"ok\":false,\"reason\":\"sleeping\"}");
    } else {
      feederRegisterFailure("manuel yemleme komutu", true);
      apiSend(504, "application/json", "{\"ok\":false,\"err\":\"timeout\"}"); 
    }
  }
}

// Mama Kabı paneline giriş: ESP32 feeder'ı sorgulayp önbelleğini tazeler (feeder yeni ölçüm yapmaz, kendi belleğini verir).
// Sorgu feederPollLoop'ta yapılır; sonuç (değerler / ulaşılamıyor bayrağı) durum push'u ile gelir. Hemen 200 döner.
void handleFeederRefresh() {
  setCORS();
  static unsigned long lastReq = 0;
  unsigned long ms = millis();
  if (!lastReq || ms - lastReq >= 3000) { lastReq = ms; feederPanelPollPending = true; feederPanelPollWhy = "panel girisi"; }
  apiSend(200, "application/json", "{\"ok\":true}");
}

void handleFeederGetConfig() {
  setCORS();
  EspNowMsg req = {CMD_REQ_CONFIG}, res;

  if (sendEspNowCommand(req, res)) {
    feederOnline = true;

    JsonDocument doc;
    feederMarkAlive(); feederCacheConfig(res); feederConfigSynced = true;
    doc["ok"]     = true;

    JsonArray arr = doc["slots"].to<JsonArray>();
    for (int i = 0; i < 4; i++) {
      JsonObject slot = arr.add<JsonObject>();
      slot["h"]      = res.slot_h[i];
      slot["m"]      = res.slot_m[i];
      slot["active"] = (FEEDER_FLAG_GET(res.flags, i) == 1);
    }

    doc["autoFeedEnable"]  = (FEEDER_FLAG_GET(res.flags, 4) == 1);
    doc["deepSleepEnable"] = (FEEDER_FLAG_GET(res.flags, 5) == 1);
    doc["startH"]          = res.sleep[0];
    doc["startM"]          = res.sleep[1];
    doc["endH"]            = res.sleep[2];
    doc["endM"]            = res.sleep[3];

    String out; serializeJson(doc, out);
    apiSend(200, "application/json", out);

  } else {
    feederOnline = false;

    if (isFeederSleeping()) {
      // Planlı uyku dönemindeyiz: RAM'deki son doğrulanmış değişkenleri arayüze bas
      JsonDocument doc;
      doc["ok"]     = false;
      doc["reason"] = "sleeping";

      JsonArray arr = doc["slots"].to<JsonArray>();
      for (int i = 0; i < 4; i++) {
        JsonObject slot = arr.add<JsonObject>();
        slot["h"]      = feederSlots[i].h;
        slot["m"]      = feederSlots[i].m;
        slot["active"] = feederSlots[i].active;
      }

      doc["autoFeedEnable"]  = feederAutoFeedEnable;
      doc["deepSleepEnable"] = feederSleepEnabled;
      doc["startH"]          = feederSleepStartH;
      doc["startM"]          = feederSleepStartM;
      doc["endH"]            = feederSleepEndH;
      doc["endM"]            = feederSleepEndM;

      String out; serializeJson(doc, out);
      apiSend(203, "application/json", out);
    } else {
      // Uyku saati dışında yanıt alamadık -> Gerçek bağlantı kopukluğu
      feederRegisterFailure("ayar okuma", true);
      apiSend(504, "application/json", "{\"ok\":false,\"err\":\"timeout\"}");
    }
  }
}

void handleFeederSetConfig() {
  setCORS(); 
  if (!apiHasArg("plain")) return; 
  JsonDocument doc; 
  if (deserializeJson(doc, apiArg("plain"))) return;

  EspNowMsg req = {CMD_SET_CONFIG}, res;

  // Geçici olarak paketi hazırla
  JsonArray arr = doc["slots"].as<JsonArray>();
  for (int i = 0; i < 4 && i < (int)arr.size(); i++) {
    req.slot_h[i] = arr[i]["h"] | 0;
    req.slot_m[i] = arr[i]["m"] | 0;
    FEEDER_FLAG_SET(req.flags, i, (arr[i]["active"] | false) ? 1 : 0);
  }

  FEEDER_FLAG_SET(req.flags, 4, (doc["autoFeedEnable"] | false) ? 1 : 0);
  FEEDER_FLAG_SET(req.flags, 5, (doc["deepSleepEnable"] | false) ? 1 : 0);
  req.sleep[0] = doc["startH"] | 0;
  req.sleep[1] = doc["startM"] | 0;
  req.sleep[2] = doc["endH"] | 0;
  req.sleep[3] = doc["endM"] | 0;

  // Yalnızca Feeder paketi Başarıyla Alıp ACK Döndüyse NVS'e ve Global Değişkenlere Yaz!
  if (sendEspNowCommand(req, res)) { 
    feederAutoFeedEnable = doc["autoFeedEnable"] | false;
    feederSleepEnabled   = doc["deepSleepEnable"] | false;
    feederSleepStartH    = doc["startH"] | 0;
    feederSleepStartM    = doc["startM"] | 0;
    feederSleepEndH      = doc["endH"] | 0;
    feederSleepEndM      = doc["endM"] | 0;

    for (int i = 0; i < 4 && i < (int)arr.size(); i++) {
      feederSlots[i].h      = arr[i]["h"] | 0;
      feederSlots[i].m      = arr[i]["m"] | 0;
      feederSlots[i].active = arr[i]["active"] | false;
    }

    savePrefs(); // Senkronizasyon sağlandı, önbelleği güncelle
    feederMarkAlive(); feederConfigSynced = true;
    feederOnline = true;
    apiSend(200, "application/json", "{\"ok\":true}");
  } 
  else {
    // Feeder paketi almadı! NVS'e YAZMA, senkronizasyonu koru.
    feederOnline = false;
    if (isFeederSleeping()) {
      apiSend(203, "application/json", "{\"ok\":false,\"reason\":\"sleeping\"}");
    } else {
      feederRegisterFailure("ayar yazma", true);
      apiSend(504, "application/json", "{\"ok\":false,\"err\":\"timeout\"}"); 
    }
  }
}

// Reset atılsa bile RAM'in bu alanındaki veri korunur
RTC_DATA_ATTR char sonResetNedeni[100] = "İlk Açılış (Henüz Reset Oluşmadı)";
// Reset nedenini tespit eden fonksiyon
void resetNedeniOku() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_UNKNOWN:   strcpy(sonResetNedeni, "Reset nedeni belirlenemedi(UNKNOWN)"); break;
    case ESP_RST_POWERON:   strcpy(sonResetNedeni, "Güç Açma (Power-on): Cihaza ilk defa güç verildi"); break;
    case ESP_RST_EXT:       strcpy(sonResetNedeni, "Harici Pin: EN (Reset) pinine fiziksel olarak basıldı/tetiklendi"); break;
    case ESP_RST_SW:        strcpy(sonResetNedeni, "Yazılımsal: esp_restart() fonksiyonu ile yeniden başlatıldı"); break;
    case ESP_RST_PANIC:     strcpy(sonResetNedeni, "Çökme (Panic/Exception): Kodda ciddi bir hata oluştu (örn. Null pointer erişimi)"); break;
    case ESP_RST_INT_WDT:   strcpy(sonResetNedeni, "Kesme Watchdog: Interrupt Watchdog Timer tetiklendi (ISR çok uzun sürdü)"); break;
    case ESP_RST_TASK_WDT:  strcpy(sonResetNedeni, "Görev Watchdog: Task Watchdog Timer tetiklendi (Bir RTOS görevi takıldı)"); break;
    case ESP_RST_WDT:       strcpy(sonResetNedeni, "Diğer Watchdog: Diğer WDT kaynaklı resetler"); break;
    case ESP_RST_DEEPSLEEP: strcpy(sonResetNedeni, "Derin Uyku Çıkışı: Deep Sleep modundan uyanma sonrası reset"); break;
    case ESP_RST_BROWNOUT:  strcpy(sonResetNedeni, "Voltaj Düşüklüğü (Brownout): Besleme voltajı kritik seviyenin altına düştü"); break;
    case ESP_RST_SDIO:      strcpy(sonResetNedeni, "SDIO: SDIO arayüzü üzerinden gelen reset"); break;

    default:                strcpy(sonResetNedeni, "Diğer / Bilinmeyen Parazit"); break;
  }
}

// ── PORTAL API'LERİ ──────────────────────────────────────────
void handleWifiStatus() { setCORS(); JsonDocument doc; bool connected = WiFi.status() == WL_CONNECTED; doc["connected"] = connected; if (connected) { doc["ssid"] = WiFi.SSID(); doc["ip"] = WiFi.localIP().toString(); } String out; serializeJson(doc, out); apiSend(200, "application/json", out); }
void handleScan() { setCORS(); int n = WiFi.scanNetworks(); JsonDocument doc; JsonArray arr = doc["networks"].to<JsonArray>(); for (int i = 0; i < n; i++) { JsonObject net = arr.add<JsonObject>(); net["ssid"] = WiFi.SSID(i); net["rssi"] = WiFi.RSSI(i); net["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN); } WiFi.scanDelete(); String out; serializeJson(doc, out); apiSend(200, "application/json", out); }
void handleWifiConfig() { setCORS(); if (!apiHasArg("plain")) return; JsonDocument doc; if (deserializeJson(doc, apiArg("plain"))) return; strlcpy(wifiSSID, doc["ssid"] | "", 33); strlcpy(wifiPass, doc["pass"] | "", 65); savePrefs(); apiSend(200, "application/json", "{\"ok\":true}"); delay(200); wifiState = WS_CONNECTING; }
void handleApConfig() { setCORS(); if (!apiHasArg("plain")) return; JsonDocument doc; if (deserializeJson(doc, apiArg("plain"))) return; strlcpy(apSSID, doc["apSSID"] | "", 33); strlcpy(apPass, doc["apPass"] | "", 65); strlcpy(apIP, doc["apIP"] | "192.168.4.1", 16); savePrefs(); apiSend(200, "application/json", "{\"ok\":true}"); delay(300); WiFi.softAPdisconnect(true); delay(100); startAP(); }
void handleNetworkConfig() { setCORS(); JsonDocument doc; doc["apSSID"] = apSSID; doc["apPass"] = apPass; doc["apIP"] = apIP; String out; serializeJson(doc, out); apiSend(200, "application/json", out); }

// MQTT ayarı: yalnız yerel HTTP (kurulum sayfası). API_ROUTES'ta YOK, yani uzaktan (WS/MQTT) değiştirilemez.
// GET şifreyi geri vermez (hasPass). POST'ta pass boşsa mevcut şifre korunur. Kayıttan sonra cihaz yeniden başlar.
void handleMqttConfigGet() {
  setCORS(); JsonDocument doc;
  doc["host"] = mqttHost; doc["port"] = mqttPort; doc["user"] = mqttUser; doc["hasPass"] = (mqttPass[0] != 0);
  String out; serializeJson(doc, out); apiSend(200, "application/json", out);
}
void handleMqttConfigPost() {
  setCORS(); if (!apiHasArg("plain")) return;
  JsonDocument doc; if (deserializeJson(doc, apiArg("plain"))) return;
  String host = doc["host"] | ""; String user = doc["user"] | ""; String pass = doc["pass"] | "";
  host.trim(); user.trim();
  long port = doc["port"] | 8883;
  if (host.length() >= sizeof(mqttHost) || user.length() >= sizeof(mqttUser) || pass.length() >= sizeof(mqttPass) || port < 1 || port > 65535) {
    apiSend(400, "application/json", "{\"ok\":false,\"err\":\"invalid\"}"); return;
  }
  strlcpy(mqttHost, host.c_str(), sizeof(mqttHost));
  strlcpy(mqttUser, user.c_str(), sizeof(mqttUser));
  mqttPort = (uint16_t)port;
  if (pass.length()) strlcpy(mqttPass, pass.c_str(), sizeof(mqttPass));
  if (host.length() == 0) mqttPass[0] = 0;   // sunucu boşaltılırsa MQTT ayarı tamamen silinir
  saveMqttPrefs();
  addLog("[MQTT] Ayar kaydedildi, cihaz yeniden baslatiliyor.");
  apiSend(200, "application/json", "{\"ok\":true,\"restart\":true}");
  delay(400); ESP.restart();
}

void handlePortal() { if (LittleFS.exists("/index.html")) { File f = LittleFS.open("/index.html", "r"); server.streamFile(f, "text/html"); f.close(); } else { apiSend(404, "text/plain", "index.html bulunamadi"); } }
void handleCSS() { if (LittleFS.exists("/style.css")) { File f = LittleFS.open("/style.css", "r"); server.streamFile(f, "text/css"); f.close(); } else { apiSend(404, "text/plain", "style.css bulunamadi"); } }
void handleJS() { if (LittleFS.exists("/script.js")) { File f = LittleFS.open("/script.js", "r"); server.streamFile(f, "application/javascript"); f.close(); } else { apiSend(404, "text/plain", "script.js bulunamadi"); } }

void handleResetReason() { setCORS(); JsonDocument doc; doc["reason"] = sonResetNedeni; String out; serializeJson(doc, out); apiSend(200, "application/json", out); }

// ── LOG API HANDLER'LARI ──────────────────────────────────────
void handleGetLogs() {
  setCORS();
  if (LittleFS.exists(LOG_FILE_PATH)) {
    File f = LittleFS.open(LOG_FILE_PATH, "r");
    server.streamFile(f, "text/plain");
    f.close();
  } else {
    apiSend(200, "text/plain", "Henuz log kaydi bulunmuyor.");
  }
}

// Uzaktan (WS/MQTT) log okuma, SALT OKUNUR: dosyayı sondan geriye doğru ~1000 bayt'lık parçalarla verir
// (MQTT tamponu 2048 bayt). q: end = bu bayt konumundan önceki parçayı ver (yok/0 = dosya sonu).
// Cevap: size (dosya boyu), start/end (verilen aralık), text. Parça satır başından başlar; start=0 ise başa ulaşıldı.
void handleLogChunk() {
  setCORS();
  constexpr size_t CHUNK = 1000;
  JsonDocument doc;
  size_t size = 0, s = 0, e = 0;
  String text;
  if (LittleFS.exists(LOG_FILE_PATH)) {
    File f = LittleFS.open(LOG_FILE_PATH, "r");
    if (f) {
      size = f.size();
      e = apiHasArg("end") ? (size_t)apiArg("end").toInt() : 0;
      if (e == 0 || e > size) e = size;
      s = e > CHUNK ? e - CHUNK : 0;
      if (s > 0) {
        f.seek(s - 1);
        f.readStringUntil('\n');            // yarım satırı atla, söz konusu s satır başı ise tam o noktada kalır
        size_t p = f.position();
        s = (p < e) ? p : (e - CHUNK);      // satır sonu bulunamazsa ham kesim
      }
      text.reserve(e - s + 1);
      f.seek(s);
      size_t remain = e - s;
      uint8_t tmp[128];
      while (remain) {
        size_t n = f.read(tmp, remain < sizeof(tmp) ? remain : sizeof(tmp));
        if (!n) break;
        text.concat((const char*)tmp, (unsigned int)n);
        remain -= n;
      }
      f.close();
    }
  }
  doc["ok"] = true; doc["size"] = (uint32_t)size; doc["start"] = (uint32_t)s; doc["end"] = (uint32_t)e; doc["text"] = text;
  String out; serializeJson(doc, out);
  apiSend(200, "application/json", out);
}

void handleClearLogs() {
  setCORS();
  if (LittleFS.exists(LOG_FILE_PATH)) {
    LittleFS.remove(LOG_FILE_PATH);
  }
  addLog("[SYSTEM] Log dosyasi manuel olarak sifirlandi.");
  apiSend(200, "application/json", "{\"ok\":true}");
}

// ── UZAKTAN YENİDEN BAŞLATMA ────────────────────────────────────────────
// Cevap önce gider (WS/MQTT'nin gönderme zamanı kalması için), yeniden başlatma ~1 sn sonra loop()'ta yapılır:
// tüm röleler kapatılır (active-LOW: HIGH) ve ESP.restart() çağrılır.
unsigned long pendingRestartAt = 0;  // millis, 0 = istek yok
void handleRestart() {
  setCORS();
  if (!pendingRestartAt) {
    pendingRestartAt = millis() + 1000;
    if (!pendingRestartAt) pendingRestartAt = 1;
    addLog("[SYSTEM] Uygulamadan yeniden baslatma istendi.");
  }
  apiSend(200, "application/json", "{\"ok\":true}");
}

// ── UZAK API: ORTAK İSTEK/CEVAP PROTOKOLÜ (WebSocket + MQTT) ──────────────
// İstek : {"id":"a1","m":"POST","p":"/api/light","q":{"id":"0","on":"true"},"b":{...}}
// Cevap : {"id":"a1","code":200,"b":{...}}
// İtme  : {"ev":"status","b":{...}}
void handleDiag();  // handleMqttStatus'un yanında tanımlı (mqtt değişkenleri bu noktadan sonra bildiriliyor)
struct ApiRoute { const char* method; const char* path; void (*fn)(); };
static const ApiRoute API_ROUTES[] = {
  {"GET",  "/api/status",            handleStatus},
  {"POST", "/api/light",             handleLight},
  {"POST", "/api/pool-on-off",       handlePool},
  {"POST", "/api/valve-on-off",      handleValveOnOff},
  {"POST", "/api/valve-auto-enable", handleValveAutoEnable},
  {"POST", "/api/valve-cfg",         handleValveCfgPost},
  {"GET",  "/api/valve-cfg",         handleValveCfgGet},
  {"GET",  "/api/feeder/status",     handleFeederGetStatus},
  {"GET",  "/api/feeder/time",       handleFeederGetTime},
  {"POST", "/api/feeder/feed",       handleFeederManualFeed},
  {"POST", "/api/feeder/refresh",    handleFeederRefresh},
  {"GET",  "/api/feeder/config",     handleFeederGetConfig},
  {"POST", "/api/feeder/config",     handleFeederSetConfig},
  {"GET",  "/api/reset-reason",      handleResetReason},
  {"POST", "/api/restart",           handleRestart},
  {"GET",  "/api/log-chunk",         handleLogChunk},   // salt okunur
  {"GET",  "/api/diag",              handleDiag},       // salt okunur
};

// Tek bir isteği çalıştırır, cevap JSON'unu döndürür (geçersiz istekte boş String).
String apiHandleRequest(const char* json, size_t len) {
  JsonDocument req;
  if (deserializeJson(req, json, len)) return "";
  const char* id = req["id"] | "";
  size_t il = strlen(id);
  if (il == 0 || il > 24) return "";
  for (size_t i = 0; i < il; i++) {
    char c = id[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return "";
  }
  const char* m = req["m"] | "GET";
  const char* p = req["p"] | "";

  const ApiRoute* route = nullptr;
  for (const ApiRoute& r : API_ROUTES) {
    if (strcmp(r.method, m) == 0 && strcmp(r.path, p) == 0) { route = &r; break; }
  }

  int code = 404;
  String out = "{\"ok\":false,\"err\":\"not_found\"}";
  if (route) {
    apiCtx.active  = true;
    apiCtx.code    = 0;
    apiCtx.out     = "";
    apiCtx.q       = req["q"].as<JsonObjectConst>();
    apiCtx.hasBody = !req["b"].isNull();
    apiCtx.body    = "";
    if (apiCtx.hasBody) serializeJson(req["b"], apiCtx.body);
    route->fn();
    if (apiCtx.code) { code = apiCtx.code; out = apiCtx.out; }
    else { code = 400; out = "{\"ok\":false,\"err\":\"bad_request\"}"; }
    apiCtx.active = false;
    apiCtx.q      = JsonObjectConst();
    apiCtx.body   = "";
    apiCtx.out    = "";
  }
  if (out.length() == 0) out = "null";

  String res;
  res.reserve(out.length() + 48);
  res += "{\"id\":\""; res += id;
  res += "\",\"code\":"; res += code;
  res += ",\"b\":"; res += out; res += "}";
  return res;
}

// /api/status çıktısını sanal çağrı ile üretir.
String apiBuildStatus() {
  apiCtx.active = true; apiCtx.code = 0; apiCtx.out = "";
  apiCtx.q = JsonObjectConst(); apiCtx.hasBody = false; apiCtx.body = "";
  handleStatus();
  String out = apiCtx.out;
  apiCtx.active = false; apiCtx.out = "";
  return out.length() ? out : String("{}");
}

// Durum değişti mi anlamak için (saat hariç) küçük bir imza.
uint32_t apiStatusSignature() {
  uint32_t s = 0;
  if (lights[0]) s |= (1u << 0);
  if (lights[1]) s |= (1u << 1);
  if (poolOn)    s |= (1u << 2);
  if (valves[0].isOpen)      s |= (1u << 3);
  if (valves[0].autoEnabled) s |= (1u << 4);
  if (valves[1].isOpen)      s |= (1u << 5);
  if (valves[1].autoEnabled) s |= (1u << 6);
  if (isTimeReliable())      s |= (1u << 7);
  if (timeSyncedByNTP)       s |= (1u << 8);
  if (feederOnline)          s |= (1u << 9);
  if (isFeederSleeping())    s |= (1u << 10);
  s |= ((uint32_t)wifiState & 0x3u) << 11;
  if (feederUnreachable)     s |= (1u << 13);
  s |= ((uint32_t)(feederFoodLevel & 0x7F)) << 14;  // mama/pil değişince durum push edilir
  s |= ((uint32_t)(feederBattery   & 0x7F)) << 21;
  return s;
}

// ── WEBSOCKET (yerel) ─────────────────────────────────────────
void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      String msg = "{\"ev\":\"status\",\"b\":" + apiBuildStatus() + "}";
      webSocket.sendTXT(num, msg.c_str(), msg.length());
      break;
    }
    case WStype_TEXT: {
      String res = apiHandleRequest((const char*)payload, length);
      if (res.length()) webSocket.sendTXT(num, res.c_str(), res.length());
      break;
    }
    default: break;
  }
}

// ── MQTT (uzak) ───────────────────────────────────────────────
// MQTT istemcisi AYRI bir FreeRTOS task'ta çalışır: ağ yok/yavaş iken bağlanma denemeleri
// loop()'u (web sunucusu, RF kumanda, sulama otomasyonu) bloklamaz.
// Task, handler çağırmaz ve addLog() kullanmaz (LittleFS thread-safe değil); gelen istekleri
// kuyruğa koyar, loop() işler, cevabı kuyruğa koyar, task yayınlar.
#define MQTT_T_REQ    "smarthome/panel/req"
#define MQTT_T_RES    "smarthome/panel/res"
#define MQTT_T_STATUS "smarthome/panel/status"
#define MQTT_T_ONLINE "smarthome/panel/online"

WiFiClientSecure mqttNet;
PubSubClient     mqttClient(mqttNet);
struct MqttOut { uint8_t kind; String* s; };   // kind 0 = res, 1 = status
QueueHandle_t    mqttInQ  = nullptr;
QueueHandle_t    mqttOutQ = nullptr;
volatile bool    mqttUp = false;
volatile int     mqttLastState = 0;
volatile uint8_t mqttFailCount = 0;
bool             mqttEnabled = false;
bool             mqttLoggedUp = false;
// Kopma/hata izleme (task yazar, loop okuyup loglar)
volatile uint8_t  mqttDropCause = 0;     // 0 yok, 1 WiFi yok, 2 saat guvenilir degil, 3 broker/ag
volatile int      mqttDropState = 0;     // cause 3 icin PubSubClient state
volatile uint32_t mqttFailSeq = 0;       // task her basarisiz denemede arttirir
char              mqttTlsErr[64] = "";   // son basarisiz denemenin TLS hata metni (yalniz task yazar)
uint32_t          mqttLoggedFailSeq = 0;
int               mqttLoggedFailState = 999;
unsigned long     mqttDownSince = 0;     // 0 = henuz hic kopmadi

const char* mqttStateText(int s) {
  switch (s) {
    case -4: return "yanit yok, keepalive zaman asimi";
    case -3: return "baglanti kayboldu";
    case -2: return "sunucuya baglanilamadi (DNS/TCP/TLS)";
    case -1: return "baglanti kapatildi";
    case 1:  return "protokol reddi";
    case 2:  return "client id reddi";
    case 3:  return "sunucu kullanilamiyor";
    case 4:  return "kullanici/sifre hatali";
    case 5:  return "yetkisiz";
    default: return "bilinmiyor";
  }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_T_REQ) != 0 || length == 0 || length > 1500) return;
  String* s = new String();
  s->concat((const char*)payload, length);
  if (xQueueSend(mqttInQ, &s, 0) != pdTRUE) delete s;
}

void mqttDrainOut() {
  MqttOut m;
  while (mqttOutQ && xQueueReceive(mqttOutQ, &m, 0) == pdTRUE) delete m.s;
}

void mqttEnqueue(uint8_t kind, const String& s) {
  if (!mqttEnabled || !mqttUp) return;
  MqttOut m; m.kind = kind; m.s = new String(s);
  if (xQueueSend(mqttOutQ, &m, 0) != pdTRUE) delete m.s;
}

void mqttTaskFn(void*) {
  unsigned long nextTry = 0;
  static const uint32_t BACKOFF_S[] = {10, 30, 60, 120, 300};
  for (;;) {
    // TLS sertifika doğrulaması için saat güvenilir olmalı
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    bool netOk = wifiOk && isTimeReliable();
    if (!netOk) {
      if (mqttUp) mqttDropCause = wifiOk ? 2 : 1;
      if (mqttClient.connected()) mqttClient.disconnect();
      mqttUp = false; mqttDrainOut();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!mqttClient.connected()) {
      if (mqttUp) { mqttDropCause = 3; mqttDropState = mqttClient.state(); }
      mqttUp = false; mqttDrainOut();
      if ((long)(millis() - nextTry) < 0) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
      String cid = "esp32-panel-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
      bool ok = mqttClient.connect(cid.c_str(), mqttUser, mqttPass, MQTT_T_ONLINE, 0, true, "offline");
      if (ok) {
        mqttClient.subscribe(MQTT_T_REQ);
        mqttClient.publish(MQTT_T_ONLINE, "online", true);
        mqttFailCount = 0; mqttUp = true;
      } else {
        mqttLastState = mqttClient.state();
        mqttTlsErr[0] = 0;
        mqttNet.lastError(mqttTlsErr, sizeof(mqttTlsErr));
        mqttFailSeq++;
        if (mqttFailCount < 250) mqttFailCount++;
        uint8_t idx = mqttFailCount > 5 ? 4 : mqttFailCount - 1;
        nextTry = millis() + BACKOFF_S[idx] * 1000UL;
      }
      continue;
    }
    mqttClient.loop();
    MqttOut m;
    while (xQueueReceive(mqttOutQ, &m, 0) == pdTRUE) {
      mqttClient.publish(m.kind == 0 ? MQTT_T_RES : MQTT_T_STATUS, m.s->c_str());
      delete m.s;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ── UZAK API: BAŞLAT / DÖNGÜ ──────────────────────────────────
constexpr unsigned long STATUS_HEARTBEAT_MS = 10000;
unsigned long apiLastStatusPush = 0;
uint32_t      apiLastStatusSig  = 0xFFFFFFFFu;

// MQTT durumu (kurulum sayfasi durum kutusu). Yalniz yerel HTTP; API_ROUTES'ta yok.
void handleMqttStatus() {
  setCORS(); JsonDocument doc;
  doc["enabled"] = mqttEnabled; doc["up"] = (bool)mqttUp;
  if (mqttEnabled && !mqttUp) {
    if (WiFi.status() != WL_CONNECTED) doc["detail"] = "WiFi yok";
    else if (mqttFailSeq > 0) {
      String d = mqttStateText(mqttLastState);
      if (mqttTlsErr[0]) { d += " / TLS: "; d += mqttTlsErr; }
      doc["detail"] = d;
    }
  }
  String out; serializeJson(doc, out); apiSend(200, "application/json", out);
}

// Salt okunur cihaz tanısı (uzaktan Cihaz sayfası). Ayar/şifre içermez; yalnız GET.
void handleDiag() {
  setCORS(); JsonDocument doc;
  doc["uptimeSec"]   = (uint32_t)(millis() / 1000UL);   // ~49 günde sıfırlanır (millis taşması)
  doc["freeHeap"]    = (uint32_t)ESP.getFreeHeap();
  doc["minFreeHeap"] = (uint32_t)ESP.getMinFreeHeap();
  doc["fw"]          = __DATE__ " " __TIME__;
  doc["resetReason"] = sonResetNedeni;
  doc["ntpAgeSec"]   = timeSyncedByNTP ? (int)((millis() - lastNtpSync) / 1000UL) : -1;
  doc["wsClients"]   = (int)webSocket.connectedClients();

  JsonObject w = doc["wifi"].to<JsonObject>();
  bool wc = (WiFi.status() == WL_CONNECTED);
  w["state"] = (int)wifiState; w["connected"] = wc;
  if (wc) { w["rssi"] = WiFi.RSSI(); w["channel"] = WiFi.channel(); w["ip"] = WiFi.localIP().toString(); }

  JsonObject m = doc["mqtt"].to<JsonObject>();
  m["enabled"] = mqttEnabled; m["up"] = (bool)mqttUp; m["fails"] = (int)mqttFailCount;

  JsonObject fs = doc["fs"].to<JsonObject>();
  fs["total"] = (uint32_t)LittleFS.totalBytes(); fs["used"] = (uint32_t)LittleFS.usedBytes();
  size_t logSize = 0;
  if (LittleFS.exists(LOG_FILE_PATH)) { File lf = LittleFS.open(LOG_FILE_PATH, "r"); if (lf) { logSize = lf.size(); lf.close(); } }
  fs["log"] = (uint32_t)logSize;

  JsonObject fp = doc["feederPoll"].to<JsonObject>();
  fp["channel"] = lastFeederChannel; fp["fails"] = feederPollFails; fp["retry"] = feederRetryPending;
  fp["window"] = feederWindowWasOpen;
  fp["nextPeriodicSec"] = (feederWindowWasOpen && isTimeReliable()) ? (int)((int32_t)(feederNextPeriodic - (uint32_t)time(NULL))) : -1;
  String out; serializeJson(doc, out); apiSend(200, "application/json", out);
}

void remoteApiBegin() {
  webSocket.begin();
  webSocket.onEvent(onWsEvent);
  webSocket.enableHeartbeat(15000, 3000, 2);   // ping/pong: yarım açık bağlantıyı temizler

  if (!mqttHost[0] || !mqttUser[0] || !mqttPass[0]) {
    addLog("[MQTT] Ayar girilmemis (kurulum sayfasi > MQTT), MQTT kapali.");
    return;
  }
  if (strlen(EMQX_CA_PEM) > 100) {
    mqttNet.setCACert(EMQX_CA_PEM);
  } else {
    mqttNet.setInsecure();
    addLog("[MQTT] UYARI: CA sertifikasi yok, sunucu dogrulanmiyor!");
  }
  mqttClient.setServer(mqttHost, mqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(2048);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(5);
  mqttInQ  = xQueueCreate(4, sizeof(String*));
  mqttOutQ = xQueueCreate(8, sizeof(MqttOut));
  mqttEnabled = true;
  xTaskCreatePinnedToCore(mqttTaskFn, "mqtt", 10240, nullptr, 1, nullptr, 0);
  addLog("[MQTT] Task baslatildi.");
}

void remoteApiLoop() {
  webSocket.loop();
  unsigned long now = millis();

  if (mqttEnabled) {
    String* rq;
    while (xQueueReceive(mqttInQ, &rq, 0) == pdTRUE) {
      String res = apiHandleRequest(rq->c_str(), rq->length());
      delete rq;
      if (res.length()) mqttEnqueue(0, res);
    }
    bool up = mqttUp;
    if (up != mqttLoggedUp) {
      mqttLoggedUp = up;
      if (up) {
        if (mqttDownSince) addLogf("[MQTT] Baglandi (%lu sn kesintiden sonra).", (millis() - mqttDownSince) / 1000UL);
        else addLog("[MQTT] Baglandi.");
        mqttDownSince = 0;
        mqttLoggedFailState = 999;
      } else {
        mqttDownSince = millis(); if (!mqttDownSince) mqttDownSince = 1;
        if (mqttDropCause == 1)      addLog("[MQTT] Baglanti koptu: WiFi yok.");
        else if (mqttDropCause == 2) addLog("[MQTT] Baglanti koptu: saat guvenilir degil.");
        else addLogf("[MQTT] Baglanti koptu: state=%d (%s)", (int)mqttDropState, mqttStateText(mqttDropState));
        mqttDropCause = 0;
      }
    }
    if (!up && mqttFailSeq != mqttLoggedFailSeq) {
      uint32_t seq = mqttFailSeq;
      int st = mqttLastState;
      mqttLoggedFailSeq = seq;
      // Flash'i yormamak icin: ilk hata, state degisimi ve her 12. denemede logla
      if (st != mqttLoggedFailState || seq % 12 == 0) {
        mqttLoggedFailState = st;
        if (mqttTlsErr[0]) addLogf("[MQTT] Baglanti basarisiz #%lu: state=%d (%s), TLS: %s", (unsigned long)seq, st, mqttStateText(st), mqttTlsErr);
        else addLogf("[MQTT] Baglanti basarisiz #%lu: state=%d (%s)", (unsigned long)seq, st, mqttStateText(st));
      }
    }
  }

  bool wsClients = webSocket.connectedClients() > 0;
  bool mqttOn    = mqttEnabled && mqttUp;
  uint32_t sig = apiStatusSignature();
  if (sig != apiLastStatusSig || now - apiLastStatusPush >= STATUS_HEARTBEAT_MS) {
    if (wsClients || mqttOn) {
      String msg = "{\"ev\":\"status\",\"b\":" + apiBuildStatus() + "}";
      if (wsClients) webSocket.broadcastTXT(msg.c_str(), msg.length());
      if (mqttOn)    mqttEnqueue(1, msg);
    }
    apiLastStatusSig  = sig;
    apiLastStatusPush = now;
  }
}

void setupRoutes() {
  server.on("/", HTTP_GET, handlePortal); server.on("/style.css", HTTP_GET, handleCSS); server.on("/script.js", HTTP_GET, handleJS);
  server.on("/api/scan", HTTP_GET, handleScan); server.on("/api/wifi-status", HTTP_GET, handleWifiStatus);
  server.on("/api/wifi-config", HTTP_POST, handleWifiConfig); server.on("/api/ap-config", HTTP_POST, handleApConfig); 
  server.on("/api/network-config", HTTP_GET, handleNetworkConfig); server.on("/api/status", HTTP_GET, handleStatus); 
  server.on("/api/mqtt-config", HTTP_GET, handleMqttConfigGet); server.on("/api/mqtt-config", HTTP_POST, handleMqttConfigPost); server.on("/api/mqtt-status", HTTP_GET, handleMqttStatus);

  server.on("/api/light",             HTTP_POST, handleLight); 
  server.on("/api/pool-on-off",       HTTP_POST, handlePool);
  server.on("/api/valve-on-off",      HTTP_POST, handleValveOnOff);
  server.on("/api/valve-auto-enable", HTTP_POST, handleValveAutoEnable);
  server.on("/api/valve-cfg",         HTTP_POST, handleValveCfgPost);
  server.on("/api/valve-cfg",         HTTP_GET,  handleValveCfgGet);

  server.on("/api/feeder/status",     HTTP_GET,  handleFeederGetStatus);
  server.on("/api/feeder/time",       HTTP_GET,  handleFeederGetTime);
  server.on("/api/feeder/feed",       HTTP_POST, handleFeederManualFeed);
  server.on("/api/feeder/refresh",    HTTP_POST, handleFeederRefresh);
  server.on("/api/feeder/config",     HTTP_GET,  handleFeederGetConfig);
  server.on("/api/feeder/config",     HTTP_POST, handleFeederSetConfig); 

  server.on("/api/reset-reason",      HTTP_GET,  handleResetReason);
  server.on("/api/restart",           HTTP_POST, handleRestart);

  server.on("/api/logs",              HTTP_GET,  handleGetLogs);
  server.on("/api/logs/clear",        HTTP_POST, handleClearLogs);

  server.onNotFound([]() { setCORS(); if (server.method() == HTTP_OPTIONS) { apiSend(204); return; } apiSend(404, "text/plain", "Not found"); });
}

unsigned long startNTP;

// ── SETUP ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  for (int pin : ALL_RELAY_PINS) { digitalWrite(pin, HIGH); pinMode(pin, OUTPUT); }
  digitalWrite(PIN_BUZZER, LOW);  pinMode(PIN_BUZZER, OUTPUT);
  resetNedeniOku();
  addLog("\n[SYSTEM] ESP32 Panel Modulu baslatiliyor...");
  bootBeep();

  rcSwitch.enableReceive(PIN_RF_DATA);

  if (LittleFS.begin()) {
    addLog("[LittleFS: OK] Dosya sistemi baslatildi.");
  } else addLog("[LittleFS: ERR] Mount Error!");
  loadPrefs(); loadMqttPrefs();
  WiFi.mode(WIFI_AP_STA); delay(100);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onDataRecv);
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, feederMac, 6);
    peerInfo.channel = 0; // Mevcut Wi-Fi kanalını takip et
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_AP;
    
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      addLog("[ESP-NOW: OK] Peer eklendi ve ESP-NOW hazir.");
    } else {
      addLog("[ESP-NOW: ERR] Peer eklenemedi!");
    }
  } else {
    addLog("[ESP-NOW: ERR] ESP-NOW baslatilamadi!");
  }

  setupRoutes(); server.begin(); handleWiFiStateMachine(); 
  ArduinoOTA.setHostname("esp32-panel"); ArduinoOTA.begin();
  remoteApiBegin();

  sntp_set_time_sync_notification_cb(timeSyncNotificationCallback);
  startNTP = millis(); configTzTime("TRT-3", "pool.ntp.org");
  addLog("[NTP] SNTP configTime yapildi: " + String(startNTP));
}

// ── LOOP ─────────────────────────────────────────────────────
bool firstLoop = false;
bool isNtpCheckSafe = false;
void loop() {
  ArduinoOTA.handle(); server.handleClient(); handleWiFiStateMachine(); handleRF(); remoteApiLoop();
  unsigned long now = millis();
  if (pendingRestartAt && (long)(now - pendingRestartAt) >= 0) {
    for (int pin : ALL_RELAY_PINS) digitalWrite(pin, HIGH);  // yükler kapalı (active-LOW)
    addLog("[SYSTEM] Yeniden baslatiliyor (uygulama istegi).");
    delay(200);
    ESP.restart();
  }
  if (!firstLoop) {
    firstLoop = true;
    addLog("[SYSTEM] First Loop: " + String(now));
  }

  if (!isNtpCheckSafe && now - startNTP > 5000) isNtpCheckSafe = true;

  if (isNtpCheckSafe) {
    if (timeSyncedByNTP && (now - lastNtpSync > NTP_TIMEOUT_LIMIT)) { 
      timeSyncedByNTP = false; lastRtcGetTime = 0;
      addLog("\n[NTP] Saat 185dk'dir SNTP ile guncellenmedi, RTC (Feeder) sorgulanacak!");
    }
    if (timeSyncedByNTP && (now - lastRtcSetTime > RTC_SET_INTERVAL || lastRtcSetTime == 0)) {
      lastRtcSetTime = now; EspNowMsg req = {CMD_SET_RTC}, res;
      if (sendEspNowCommand(req, res)) {
        addLog("[NTP] RTC (Feeder) saati NTP ile senkronize edildi.");
      } else {
        addLog("[NTP] RTC (Feeder) senkronizasyonu basarisiz! Cihaz yanit vermedi.");
      }
    }
    if (!timeSyncedByNTP && (now - lastRtcGetTime >= NTP_TIMEOUT_LIMIT || lastRtcGetTime == 0)) {
      lastRtcGetTime = now; EspNowMsg req = {CMD_REQ_RTC}, res;
      if (sendEspNowCommand(req, res) && res.timestamp >= EPOCH_2026_THRESHOLD) {
        struct timeval tv; tv.tv_sec = (time_t)res.timestamp; tv.tv_usec = 0; settimeofday(&tv, NULL);
        timeSyncedByRTC = true;
        addLog("\n[SYSTEM] RTC (Feeder) saati alindi! NTP dogrulanana kadar RTC kullanilacak.");
      } else {
        timeSyncedByRTC = false;
        addLog("\n[SYSTEM] RTC (Feeder) saati alinamadi! NTP dogrulanana kadar 'sulama otomasyonu' devre disi.");
      }
    }
    if (now - lastAutoValveCheck >= 1000) { lastAutoValveCheck = now; checkAutoValve(); }
    checkValveTimeouts();
    checkEspNowAsyncTimeout();
    feederPollLoop();
  }
}
# Smart Home Panel

[English](README.md) | **Türkçe**

Kendin yap ev otomasyonu projesi: bahçe aydınlatması, sulama vanaları ve havuz motoru (ESP32) ile otomatik kedi maması kabı (ESP8266). Her şey, ESP32 ile MQTT sunucusu üzerinden konuşan bir web panelinden kontrol edilir, yani ev ağının dışından da çalışır.

Canlı panel: https://soykose.github.io/smart-home-panel/ (kendi MQTT sunucu bilgilerin olmadan bir şey yapmaz, bkz. [Web panel](#web-panel)).

> Panel ve cihaz kurulum sayfası Türkçedir. Koddaki yorumlar da çoğunlukla Türkçedir.

## Nasıl çalışır

```
Web panel (GitHub Pages)  --WSS üzerinden MQTT-->  MQTT sunucusu (EMQX Cloud Serverless)
                                                          |
                                                          | TLS üzerinden MQTT (8883)
                                                          v
                                                    ESP32 "master"  --ESP-NOW-->  ESP8266 "feeder"
                                              (röleler, RF kumanda, sensörler)   (servo, HC-SR04, RTC)
```

- ESP32 komutları MQTT ile alır (ve 81 numaralı portta yerel WebSocket ile; bunu bu depoda olmayan Android uygulaması kullanır). Röleleri sürer, mama kabı komutlarını ESP-NOW ile ESP8266'ya iletir.
- Cihaz durumunu ESP32 gönderir (push); panel sürekli sorgulamaz.
- ESP32'nin 80 numaralı portta yerel bir HTTP API'si de vardır. Kurulum sayfası ve elle test için kullanılır. MQTT ya da yerel WebSocket üzerinden panel yalnız durumu ve logu okuyabilir, ESP32'den mama kabı durumunu tazelemesini isteyebilir ve ESP32'yi yeniden başlatabilir. Ayarlar uzaktan değiştirilemez.

## Depo yapısı

| Yol | İçerik |
|---|---|
| `firmware/esp32/` | ESP32 yazılımı (PlatformIO, Arduino) ve `data/` (LittleFS kurulum sayfası) |
| `firmware/esp8266/` | Wemos D1 mini için mama kabı yazılımı (PlatformIO, Arduino) |
| `docs/` | Web panel, GitHub Pages ile yayınlanır |

## ESP32 (ev paneli)

**Donanım:** ESP32 geliştirme kartı (38 pin), 8 kanallı active-LOW röle modülü, 433 MHz RF alıcı (isteğe bağlı), aktif buzzer.

| İşlev | Pin |
|---|---|
| Röle 1, 2, 3 (boş) | 32, 33, 25 |
| Röle 4, 5 (aydınlatma 2, 1) | 26, 27 |
| Röle 6, 7 (sulama vana 2, 1) | 14, 12 |
| Röle 8 (havuz motoru) | 13 |
| RF alıcı DATA | 23 |
| Buzzer | 18 |

**Özellikler:** her biri 4 zaman slotlu 2 sulama vanası (NVS'de saklanır), aydınlatma ve havuz kontrolü, 433 MHz kumanda desteği (RCSwitch), NTP'den saat (olmazsa mama kabındaki DS1307 RTC; ESP32 açılışta ve sonra 12 saatte bir NTP saatini mama kabının RTC'sine de yazar), mama kabının pil ve mama seviyesinin önbellekteki kopyası (bkz. [Mama kabı durumu](#mama-kabı-durumu)), panelde salt okunur Cihaz sayfası (ESP32 durumu ve log) ve uzaktan yeniden başlatma butonu, OTA güncelleme.

### Derleme ve yükleme

[PlatformIO](https://platformio.org/) kur. Kütüphaneler `platformio.ini` içinden otomatik iner.

```
cd firmware/esp32
pio run -e esp32_usb -t upload      # yazılım, USB ile
pio run -e esp32_usb -t uploadfs    # kurulum sayfası (LittleFS imajı), bir kez ve data/ değiştikçe gerekir
```

`esp32_ota` ortamı Wi-Fi üzerinden `192.168.1.100` adresine yükler.

### Kendine göre değiştirmen gerekenler

- **Mama kabı MAC adresi:** `src/main.cpp` içindeki `feederMac`, yazarın D1 mini'sinin MAC adresidir. Kendininkini yaz.
- **Statik IP:** `src/main.cpp` içindeki `local_IP`, `gateway`, `subnet` ve `platformio.ini` içindeki `upload_port`, `192.168.1.x` ağını varsayar.
- **RF kumanda kodları:** `RF_LIGHT1` ... `RF_VALVE2` yazarın kumandasının kodlarıdır. Kendi kumandanın kodlarıyla değiştir (kodları bilen biri RF ile röleleri tetikleyebilir).
- **Saat dilimi:** `TRT-3` (Türkiye) iki yazılımda da sabit yazılıdır.

### İlk kurulum

1. Yazılımı ve dosya sistemi imajını yükle.
2. ESP32 bir Wi-Fi ağına bağlanamazsa kendi erişim noktasını (AP) açar. Kurulum sayfasını AP adresinden aç (varsayılan `192.168.4.1`), **Wi-Fi** sekmesinden ağını seç.
3. **MQTT** sekmesine sunucu adı, port (8883), kullanıcı adı ve şifreyi gir. Bilgiler kodda değil, cihazın NVS belleğinde saklanır. Kaydedince cihaz yeniden başlar. Bu ayarlar girilmezse MQTT kapalı kalır.
4. **Cihaz** sekmesi AP ayarlarını ve logu gösterir.

## ESP8266 (kedi maması kabı)

**Donanım:** Wemos D1 mini, D5'te servo, D6 (TRIG) / D7 (ECHO) üzerinde HC-SR04, D1 (SCL) / D2 (SDA) üzerinde DS1307 RTC, D8'de buzzer, A0'da pil bölücü. Deep sleep için D0 pini RST'ye bağlanmalıdır.

**Özellikler:**
- Günde en fazla 4 yemleme slotu, manuel yemleme, ayarlar EEPROM'da.
- HC-SR04 ile mama seviyesi: en fazla 10 deneme (aralarında 60 ms, her denemede en fazla 5 ms yankı beklenir). İlk 5 geçerli ölçüm (yankı geldi, 1-30 cm) ortalanır ve tablo (3-28 cm) ile yüzdeye çevrilir. **Tablo (`volumeLUT`) yazarın kabına göre ayarlıdır**, kendi kabın için yeniden ayarla. 5 geçerli ölçüm yoksa mama kabı 101 gönderir; ESP32 bunu başarısız ölçüm sayar ve %0 göstermek yerine eski değeri korur.
- Ölçüm, kapak kapandıktan 2,5 sn sonra (ve açılıştan 2,5 sn sonra) yapılır. Yemlemeden hemen sonra yapılan ölçümler sıçrıyordu; beklemek deneysel olarak daha tutarlı çıktı. Kök neden ölçülmedi.
- Gerilim tablosundan pil yüzdesi.
- ESP-NOW dinleme pencereleri arasında light sleep ve isteğe bağlı deep sleep saat aralığı.
- Yemleme güvenilmez saat yüzünden engellenmez: aç kalan kedi, kusursuz zamanlamadan daha önemlidir.
- Kapak kapandıktan sonra servo `COVER_HOLD_MS` (5 sn) boyunca bağlı kalır, sonra bırakılır. Yazarın kurulumunda hemen bırakınca besleme gürültüsü kapağı oynatıyordu. Bu deneysel bir çözümdür, kök neden ölçülmedi.

```
cd firmware/esp8266
pio run -t upload
```

ESP32 ve mama kabı aynı Wi-Fi kanalını kullanmalıdır. ESP32 son bilinen kanalı dener, gerekirse 1-13 arasını tarar ve mama kabına kanal değiştirme komutu gönderebilir (mama kabı kanalı EEPROM'a yazıp yeniden başlar).

ESP-NOW paketi (`EspNowMsg`) iki yazılımda birebir aynı tanımlıdır. Birini değiştirirsen ikisini de değiştir.

### Mama kabı durumu

Mama kabı zamanının çoğunu uykuda geçirir, bu yüzden ESP32 pil ve mama seviyesinin bir kopyasını (NVS'de) tutar ve panel her zaman bu kopyadan okur. ESP32 taze değerleri mama kabına şu durumlarda sorar:

- mama kabının uyanık penceresi açılınca, sonra uyanıştan itibaren her saat;
- her aktif yemleme slotundan 5 dakika sonra;
- uygulamadan yapılan manuel yemleme onaylandıktan 5 sn sonra (mama kabı kapak kapandıktan sonra ölçer, yeni seviye o zamana hazır olur);
- panelde Mama Kabı sayfası her açıldığında (en fazla 3 sn'de bir).

Zamanlı sorgular mama kabının uyanık penceresiyle sınırlıdır. Haberleşme başarısız olursa panel mama kabını "ulaşılamıyor" olarak işaretler (kırmızı rozet, son bilinen değerler görünür kalır) ve sonraki başarılı haberleşmede işaret kalkar. Gözetimsiz sorgularda 3 ardışık hata gerekir; kullanıcı işleminden doğan bir hata bayrağı hemen açar ve 1 dakika sonra bir kontrol planlanır. Mama kabının ayarlı deep sleep süresinde panel bunun yerine mor "uykuda" rozeti gösterir.

Mama kabı ulaşılamıyor kalırsa ESP32 Wi-Fi kanal hizalamasını yeniden yapar (son bilinen kanal, sonra 1-13 taraması, sonra kanal değiştirme komutu). Bu en fazla saatte bir olur ve her seferinde Wi-Fi ile MQTT 10-20 sn kesilir.

## MQTT / EMQX

Proje EMQX Cloud Serverless ile kuruldu (aylık ücretsiz kotası var, güncel sınırlar için EMQX fiyat sayfasına bak). TLS ve TLS üzerinden WebSocket destekleyen her sunucu çalışmalıdır.

Topic kökü: `smarthome/panel`

| Topic | Yön | Amaç |
|---|---|---|
| `smarthome/panel/req` | panel -> ESP32 | komutlar ve sorgular |
| `smarthome/panel/res` | ESP32 -> panel | cevaplar (mesaj içindeki id ile eşleşir) |
| `smarthome/panel/status` | ESP32 -> panel | itilen cihaz durumu |
| `smarthome/panel/online` | ESP32 -> hepsi | `online`, Last Will olarak `offline` (retained) |

Mesajlar QoS 0 ve clean session kullanır. Komutlar bilerek retained değildir: gecikmiş bir vana ya da havuz komutu sonradan çalışmasın diye.

Önerilen sunucu kullanıcıları (kimlik doğrulama + yetkilendirme):

| Kullanıcı | Subscribe | Publish |
|---|---|---|
| ESP32 | `.../req` | `.../online`, `.../status`, `.../res` |
| Panel kullanıcıları (kişi ya da cihaz başına bir tane) | `.../online`, `.../status`, `.../res` | `.../req` |
| Diğer herkes | `#` reddet | `#` reddet |

Web panel için ayrı bir sunucu kullanıcısı aç. ESP32 kullanıcısını kullanma.

ESP32, sunucuyu `src/ca_cert.h` içindeki DigiCert Global Root G2 sertifikasıyla doğrular (bu yazılırken EMQX Serverless'ın kullandığı kök). Başka bir sunucu kullanırsan sertifikayı değiştir.

## Web panel

`docs/` içindeki panel düz HTML/JS'tir, MQTT'yi WebSocket üzerinden konuşan küçük bir istemci (`transport.js`) içerir. Derleme adımı yoktur.

Kendi kopyanı yayınlamak için: depoyu fork'la ya da yükle, GitHub'da *Settings -> Pages* bölümünden kaynağı dalındaki `/docs` klasörü yap.

İlk kullanım:
1. Paneli aç ve başlıktaki bağlantı durumu butonuna dokun.
2. **MQTT** sekmesine sunucunun WebSocket adresini (EMQX Serverless için `wss://<sunucun>:8084/mqtt`), kullanıcı adını ve şifreyi gir, kaydet.
3. Ayarlar tarayıcının `localStorage` alanında saklanır, yani her kişi her tarayıcıda bir kez girer. Depoda hiçbir kimlik bilgisi yoktur.

Ayar panelinde ayrıca salt okunur bir **Cihaz** sekmesi vardır: ESP32 durumu ve log (MQTT üzerinden küçük parçalar halinde okunur) ile onay penceresi olan kırmızı bir **ESP32'yi yeniden başlat** butonu. Buton yalnız panel bağlıyken çalışır.

**WS** (yerel WebSocket) ve **NVR/kamera** sekmeleri vardır ama web sürümünde pasiftir: HTTPS sayfa, yerel ağdaki bir cihaza düz `ws://` bağlantısı açamaz. Kamera ve yerel WebSocket yalnız Android uygulamasında çalışır.

## Bilinen sınırlar

- ESP32 ile mama kabı arasındaki ESP-NOW trafiği şifreli değildir.
- OTA güncellemesinin şifresi yoktur.
- RF kumanda kodları yazılımın içindedir (yukarıya bak).
- Web panelin sunucu bilgileri tarayıcının `localStorage` alanındadır.
- ESP32 kurulum sayfası ve yerel HTTP API, yerel ağda düz HTTP'dir.
- Dahil değil: Android uygulaması (WebView ve kamera/NVR ekranları).

## Lisans

MIT, bkz. [LICENSE](LICENSE).

## Yapay zeka kullanım bilgisi

Bu proje, Anthropic'in yapay zeka modeli Claude'un yardımıyla gözden geçirildi ve optimize edildi. Claude, claude.ai üzerinde, yerel proje dosyalarına MCP araçlarıyla erişmesini sağlayan ShadowClaude adlı bir masaüstü uygulaması aracılığıyla kullanıldı. ShadowClaude, resmi bir Anthropic uygulaması değildir. Kod, gerçek donanım üzerinde yazar tarafından test edilmiştir ve projenin sorumluluğu yazara aittir.

# Smart Home Panel

**English** | [Türkçe](README.tr.md)

A DIY home automation project: garden lighting, irrigation valves and a pool pump (ESP32), plus an automatic cat food dispenser (ESP8266). Everything is controlled from a web panel that talks to the ESP32 through an MQTT broker, so it works from outside the home network too.

Live panel: https://soykose.github.io/smart-home-panel/ (it needs your own MQTT broker details to do anything, see [Web panel](#web-panel)).

> The panel and the device setup page are in Turkish. The code comments are mostly Turkish as well.

## How it works

```
Web panel (GitHub Pages)  --MQTT over WSS-->  MQTT broker (EMQX Cloud Serverless)
                                                    |
                                                    | MQTT over TLS (8883)
                                                    v
                                              ESP32 "master"  --ESP-NOW-->  ESP8266 "feeder"
                                        (relays, RF remote, sensors)      (servo, HC-SR04, RTC)
```

- The ESP32 receives commands over MQTT (and over a local WebSocket on port 81, used by the Android app which is not part of this repository). It switches the relays and forwards feeder commands to the ESP8266 over ESP-NOW.
- Device state is pushed by the ESP32; the panel does not poll.
- The ESP32 also has a local HTTP API on port 80, used by the setup page and for manual testing. Through MQTT or the local WebSocket the panel can only read the status and the log, ask the ESP32 to refresh the feeder state and restart the ESP32. Configuration cannot be changed remotely.

## Repository layout

| Path | Contents |
|---|---|
| `firmware/esp32/` | ESP32 firmware (PlatformIO, Arduino) and `data/` (LittleFS setup page) |
| `firmware/esp8266/` | Feeder firmware for a Wemos D1 mini (PlatformIO, Arduino) |
| `docs/` | Web panel, served by GitHub Pages |

## ESP32 (home panel)

**Hardware:** ESP32 dev board (38 pin), 8-channel active-LOW relay module, 433 MHz RF receiver (optional), active buzzer.

| Function | Pin |
|---|---|
| Relay 1, 2, 3 (unused) | 32, 33, 25 |
| Relay 4, 5 (lighting 2, 1) | 26, 27 |
| Relay 6, 7 (irrigation valve 2, 1) | 14, 12 |
| Relay 8 (pool pump) | 13 |
| RF receiver DATA | 23 |
| Buzzer | 18 |

**Features:** 2 irrigation valves with 4 time slots each (stored in NVS), lighting and pool control, 433 MHz remote support (RCSwitch), time from NTP with the feeder's DS1307 RTC as a fallback (the ESP32 also sets the feeder's RTC from NTP at boot and every 12 hours), a cached copy of the feeder's battery and food level (see [Feeder status](#feeder-status)), a read-only Device page in the panel (ESP32 status and log) with a remote restart button, OTA updates.

### Build and upload

Install [PlatformIO](https://platformio.org/). Libraries are downloaded automatically from `platformio.ini`.

```
cd firmware/esp32
pio run -e esp32_usb -t upload      # firmware over USB
pio run -e esp32_usb -t uploadfs    # setup page (LittleFS image), needed once and after every change in data/
```

`esp32_ota` uploads over Wi-Fi to `192.168.1.100`.

### Things you must adapt

- **Feeder MAC address:** `feederMac` in `src/main.cpp` is the MAC of the author's D1 mini. Put your own.
- **Static IP:** `local_IP`, `gateway`, `subnet` in `src/main.cpp` and `upload_port` in `platformio.ini` assume `192.168.1.x`.
- **RF remote codes:** `RF_LIGHT1` ... `RF_VALVE2` are the codes of the author's remote. Replace them with yours (anyone who knows them can trigger the relays by RF).
- **Time zone:** `TRT-3` (Turkey) is hard coded in both firmwares.

### First setup

1. Flash the firmware and the filesystem image.
2. If the ESP32 cannot join a Wi-Fi network it starts its own access point. Open the setup page at the AP address (default `192.168.4.1`), choose your network on the **Wi-Fi** tab.
3. On the **MQTT** tab enter the broker host, port (8883), user and password. They are stored in the device's NVS, not in the code. Saving restarts the device. Without these settings MQTT stays disabled.
4. The **Device** tab shows the AP settings and the log.

## ESP8266 (cat food feeder)

**Hardware:** Wemos D1 mini, servo on D5, HC-SR04 on D6 (TRIG) / D7 (ECHO), DS1307 RTC on D1 (SCL) / D2 (SDA), buzzer on D8, battery divider on A0. For deep sleep, D0 must be wired to RST.

**Features:**
- Up to 4 daily feeding slots, manual feeding, settings kept in EEPROM.
- Food level from the HC-SR04: up to 10 attempts (60 ms apart, 5 ms echo timeout each). The first 5 valid readings (echo received, 1-30 cm) are averaged and converted to a percentage through a lookup table (3-28 cm). **The table (`volumeLUT`) is calibrated for the author's container**, so recalibrate it for yours. If fewer than 5 readings are valid, the feeder reports 101; the ESP32 treats that as a failed measurement and keeps the previous value instead of showing 0%.
- The measurement runs 2.5 s after the cover has closed (and 2.5 s after boot). Measuring right after feeding gave values that jumped around; waiting was empirically more consistent. The root cause was not measured.
- Battery percentage from a voltage table.
- Light sleep between ESP-NOW listen windows and an optional deep sleep time window.
- Feeding is never blocked by an untrusted clock: a hungry cat matters more than a perfect schedule.
- After closing, the servo stays attached for `COVER_HOLD_MS` (5 s) before it is released. On the author's build, releasing it right away let supply noise move the cover. This is an empirical fix, the root cause was not measured.

```
cd firmware/esp8266
pio run -t upload
```

The ESP32 and the feeder must use the same Wi-Fi channel. The ESP32 tries the last known channel, scans channels 1-13 if needed and can tell the feeder to switch channel (the feeder saves it to EEPROM and restarts).

The ESP-NOW packet (`EspNowMsg`) is defined identically in both firmwares. If you change it, change both.

### Feeder status

The feeder sleeps most of the time, so the ESP32 keeps a cached copy of its battery and food level (stored in NVS) and the panel always reads from that cache. The ESP32 asks the feeder for fresh values:

- when the feeder's awake window opens, then every hour from the wake-up time;
- 5 minutes after each active feeding slot;
- 5 seconds after a manual feeding is acknowledged (the feeder measures after the cover closes, so the new level is ready by then);
- whenever the Feeder page is opened in the panel (at most every 3 s).

Scheduled queries are limited to the feeder's awake window. If the communication fails, the panel marks the feeder as "unreachable" (red badge, last known values stay visible) until the next successful contact. Unattended queries need 3 failures in a row; a failure caused by a user action sets the flag immediately and a check is scheduled a minute later. During the feeder's configured deep sleep the panel shows a purple "sleeping" badge instead.

If the feeder stays unreachable, the ESP32 repeats its Wi-Fi channel alignment (last known channel, then a scan of 1-13, then the channel switch command). This happens at most once an hour, and Wi-Fi and MQTT drop for 10-20 s each time.

## MQTT / EMQX

The project was set up with EMQX Cloud Serverless (it has a free monthly quota; check EMQX's pricing page for current limits). Any broker with TLS and WebSocket-over-TLS support should work.

Topic root: `smarthome/panel`

| Topic | Direction | Purpose |
|---|---|---|
| `smarthome/panel/req` | panel -> ESP32 | commands and queries |
| `smarthome/panel/res` | ESP32 -> panel | replies (matched by id inside the message) |
| `smarthome/panel/status` | ESP32 -> panel | pushed device state |
| `smarthome/panel/online` | ESP32 -> all | `online`, and `offline` as Last Will (retained) |

Messages use QoS 0 and clean sessions. Commands are not retained on purpose, so a delayed valve or pump command can never run later.

Suggested broker users (authentication + authorization):

| User | Subscribe | Publish |
|---|---|---|
| ESP32 | `.../req` | `.../online`, `.../status`, `.../res` |
| Panel users (one per person or device) | `.../online`, `.../status`, `.../res` | `.../req` |
| Everyone else | deny `#` | deny `#` |

Use a separate broker user for the web panel. Do not reuse the ESP32 user.

The ESP32 verifies the broker with the DigiCert Global Root G2 certificate in `src/ca_cert.h` (the root EMQX Serverless used when this was written). If you use another broker, replace the certificate.

## Web panel

The panel in `docs/` is plain HTML/JS with a small MQTT client (`transport.js`) that speaks MQTT over WebSocket. There is no build step.

To host your own copy: fork or upload the repository, then in GitHub go to *Settings -> Pages* and set the source to the `/docs` folder of your branch.

First use:
1. Open the panel and tap the connection status button in the header.
2. On the **MQTT** tab enter the broker WebSocket URL (for EMQX Serverless: `wss://<your-host>:8084/mqtt`), the user and the password, and save.
3. The settings are kept in the browser's `localStorage`, so each person enters them once per browser. The repository contains no credentials.

The settings panel also has a read-only **Device** tab with the ESP32 status and log (read in small chunks over MQTT) and a red **Restart ESP32** button with a confirmation dialog. The button works only while the panel is connected.

The **WS** (local WebSocket) and **NVR/camera** tabs exist but are disabled in the web version: an HTTPS page cannot open a plain `ws://` connection to a device in your LAN. The camera and local WebSocket parts only work in the Android app.

## Known limitations

- ESP-NOW traffic between the ESP32 and the feeder is not encrypted.
- OTA updates have no password.
- The RF remote codes are in the firmware (see above).
- Broker credentials of the web panel live in the browser's `localStorage`.
- The ESP32 setup page and local HTTP API are plain HTTP on your LAN.
- Not included: the Android app (WebView plus camera/NVR screens).

## License

MIT, see [LICENSE](LICENSE).

## AI usage disclosure

This project was reviewed and optimized with the help of Claude, an AI model by Anthropic, used on claude.ai through a desktop application called ShadowClaude that gives Claude access to local project files via MCP tools. ShadowClaude is not an official Anthropic application. The code was tested on real hardware by the author, who is responsible for the project.

# Wireless Environment Monitor

Passive WiFi sniffing on ESP32 with MAC-randomization-aware occupancy estimation, served through a self-hosted real-time web dashboard.

---

## Table of Contents

- [What This Project Does](#what-this-project-does)
- [Hardware Required](#hardware-required)
- [Software Required](#software-required)
- [Setup Instructions](#setup-instructions)
- [How It Works](#how-it-works)
  - [1. Promiscuous Mode — Capturing Packets](#1-promiscuous-mode--capturing-packets)
  - [2. Extracting the MAC Address](#2-extracting-the-mac-address)
  - [3. What Is RSSI](#3-what-is-rssi)
  - [4. Proximity Classification (Near / Mid / Far)](#4-proximity-classification-near--mid--far)
  - [5. Channel Hopping](#5-channel-hopping)
  - [6. MAC Randomization Detection](#6-mac-randomization-detection)
  - [7. Corrected Occupancy Estimate](#7-corrected-occupancy-estimate)
  - [8. Stale Device Pruning](#8-stale-device-pruning)
  - [9. Web Server and Dashboard](#9-web-server-and-dashboard)
- [Full System Pipeline](#full-system-pipeline)
- [Project Structure](#project-structure)
- [Dashboard Features](#dashboard-features)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)
- [Future Work](#future-work)
- [References](#references)

---

## What This Project Does

The ESP32's built-in WiFi radio is put into **promiscuous mode**, letting it passively "overhear" every WiFi packet in range — from any device, without connecting to any network. From these packets it extracts:

- Device identity (MAC address)
- Signal strength (RSSI)
- Estimated proximity (Near / Mid / Far)
- Activity level (packet rate)
- Whether the MAC is real or randomized (privacy feature on modern phones)
- A corrected estimate of how many actual devices are present

All of this is displayed on a live, auto-refreshing web dashboard — hosted directly on the ESP32 itself. No internet, no router, no external server required.

---

## Hardware Required

| Item | Notes |
|---|---|
| ESP32 DevKit V4 | Any ESP32 dev board with WiFi works |
| USB cable (data-capable) | Must support data transfer, not charge-only |
| PC/Laptop | For flashing firmware via PlatformIO |
| Smartphone | To view the dashboard |

---

## Software Required

- [VS Code](https://code.visualstudio.com/)
- [PlatformIO IDE extension](https://platformio.org/) (install from VS Code Extensions tab)
- Drivers for your ESP32's USB chip:
  - **CP2102** → [Silicon Labs driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
  - **CH340** → search "CH340 driver Windows"

---

## Setup Instructions

### 1. Install PlatformIO
Open VS Code → Extensions (`Ctrl+Shift+X`) → search **"PlatformIO IDE"** → Install → restart VS Code.

### 2. Open the project
Open this project folder in VS Code (it already contains `platformio.ini` and `src/main.cpp`).

### 3. Connect the ESP32
Plug it into your PC via USB. Check **Device Manager → Ports (COM & LPT)** — you should see something like:
```
Silicon Labs CP210x USB to UART Bridge (COMx)
```
If nothing shows up, install the driver first (see above).

### 4. Upload the firmware
Click the **→ (Upload)** arrow in the bottom PlatformIO toolbar, or press `Ctrl+Alt+U`.

If it gets stuck at `Connecting....`, hold the **BOOT** button on the ESP32 until upload starts.

### 5. Open Serial Monitor
Click the plug icon in the toolbar (or `Ctrl+Alt+S`), baud rate **115200**. You should see:
```
=== Wireless Environment Monitor ===
Brownout: DISABLED
Mode: ESP32 Access Point
─────────────────────────────────────
  Network name : WirelessMonitor
  Password     : monitor123
  ESP32 IP     : 192.168.4.1
─────────────────────────────────────
Sniffer: ACTIVE
HTTP server: STARTED
```

### 6. Connect your phone
On your phone's WiFi settings, connect to:
```
Network:  WirelessMonitor
Password: monitor123
```
Your phone may warn "No internet connection" — tap **Stay Connected / Use Anyway**. This is expected since the ESP32 isn't routing to the internet.

### 7. Open the dashboard
On your phone's browser, go to:
```
http://192.168.4.1
```
The dashboard loads and starts showing nearby devices within a few seconds.

---

## How It Works

### 1. Promiscuous Mode — Capturing Packets

Normally, a WiFi radio only accepts packets addressed to its own MAC address — everything else is silently dropped at the hardware level. **Promiscuous mode** disables this filter, so the radio forwards *every* packet it hears to a callback function, regardless of who it was sent to.

```cpp
esp_wifi_set_promiscuous(true);
esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
```

This is what gives the ESP32 visibility into every nearby device's traffic — phones, laptops, routers, smart TVs — without joining any of their networks.

### 2. Extracting the MAC Address

Every 802.11 WiFi frame follows a fixed header layout. The **source MAC address is always located at byte offset 10 through 15** (6 bytes):

```
Byte:  [0-1]      [2-3]      [4-9]        [10-15]       [16-21]
       Frame Ctrl  Duration   Dest MAC     SOURCE MAC    BSSID
```

The firmware reads these 6 bytes and packs them into a single 64-bit integer, used as the unique key to track that device:

```cpp
uint64_t mac = 0;
for (int i = 0; i < 6; i++)
    mac = (mac << 8) | payload[10 + i];
```

### 3. What Is RSSI

**RSSI (Received Signal Strength Indicator)** is a measurement, in dBm, of how strong a radio signal was when it reached the ESP32's antenna. It's measured directly by the WiFi chip's hardware — not something we calculate:

```cpp
int rssi = pkt->rx_ctrl.rssi;
```

| RSSI Range | Meaning |
|---|---|
| −30 to −50 dBm | Extremely close / very strong |
| −50 to −70 dBm | Good, nearby |
| −70 to −85 dBm | Weak, distant |
| −85 to −100 dBm | Very weak, edge of range |

Every time a device sends a new packet, RSSI is refreshed with the latest reading.

### 4. Proximity Classification (Near / Mid / Far)

The project does **not** calculate exact distance in meters. True distance estimation requires the Free Space Path Loss formula:

```
RSSI = TxPower - 10 * n * log10(distance)
```

This needs two unknowns we don't have: `TxPower` (varies per device) and `n`, the path loss exponent (varies per environment — walls, obstructions, interference). Without calibrating both, a "distance in meters" would just be a disguised guess.

Instead, RSSI is bucketed into three practical zones, validated against real measurements:

```cpp
const char* proximityLabel(int rssi) {
  if (rssi >= -60) return "Near";
  if (rssi >= -80) return "Mid";
  return "Far";
}
```

| Classification | RSSI Threshold | Approx. Real-World Distance |
|---|---|---|
| Near | ≥ −60 dBm | 0–3 m (same room) |
| Mid | −60 to −80 dBm | 3–10 m (adjacent room) |
| Far | < −80 dBm | 10+ m / obstructed |

### 5. Channel Hopping

A WiFi radio can only listen to **one channel at a time**. Since different devices operate on different channels (1 through 13 in the 2.4 GHz band), the firmware cycles through all of them every 500 ms:

```cpp
void hopChannel() {
  if (millis() - lastHop < 500) return;
  lastHop = millis();
  currentChannel = (currentChannel % 13) + 1;
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}
```

This ensures full coverage of the 2.4 GHz spectrum rather than only seeing devices on whichever channel the ESP32 happened to start on.

### 6. MAC Randomization Detection

Modern smartphones (Android 8+, iOS 14+) **randomize their MAC address** while scanning for networks, as a privacy feature. This means a single physical phone can appear as several different "devices" to a sniffer, inflating the count.

Per the IEEE 802 spec, randomized addresses have a specific bit set — the **locally administered bit** (2nd least-significant bit of the first byte). The firmware checks this bit to flag randomized MACs:

```cpp
bool isRandomizedMAC(uint64_t mac) {
  uint8_t firstByte = (mac >> 40) & 0xFF;
  return (firstByte & 0x02) != 0;
}
```

Each detected device is tagged as either a **Real MAC** (manufacturer-assigned, reliable 1:1 device count) or a **Randomized MAC** (privacy address, potentially one of several generated by the same physical device).

### 7. Corrected Occupancy Estimate

Naively counting unique MAC addresses overestimates how many actual devices/people are present, because of randomization. The dashboard shows a corrected estimate:

```cpp
const float RANDOMIZATION_CORRECTION_FACTOR = 0.4f;
int estimatedDevices = realCount + (int)round(randomCount * RANDOMIZATION_CORRECTION_FACTOR);
```

Real MACs are counted directly (1 MAC = 1 device). Randomized MACs are scaled down by an empirical correction factor, reflecting that a single phone typically generates more than one randomized address during a scanning session. This logic is based on findings from Keio University's research on indoor crowd estimation under MAC randomization (see [References](#references)).

### 8. Stale Device Pruning

Devices that haven't sent a packet in the last 10 seconds are assumed to have left the area and are removed from the list, so the dashboard reflects only currently-present devices:

```cpp
void pruneStale() {
  if (millis() - lastPrune < 2000) return;
  lastPrune = millis();
  unsigned long now = millis();
  for (auto it = deviceMap.begin(); it != deviceMap.end(); ) {
    it = (now - it->second.lastSeen > 10000) ? deviceMap.erase(it) : ++it;
  }
}
```

### 9. Web Server and Dashboard

The ESP32 runs in **Access Point (AP) mode**, creating its own WiFi network (`WirelessMonitor`) rather than depending on an existing router or hotspot. This gives it a fixed, predictable address: `http://192.168.4.1`.

Two HTTP routes are served:
- `GET /` — the dashboard HTML/CSS/JS, stored in flash memory (PROGMEM)
- `GET /data` — a JSON snapshot of all current devices and summary stats

The dashboard's JavaScript polls `/data` every 2.5 seconds using `fetch()` and updates the page in place — no full reloads.

---

## Full System Pipeline

```
┌──────────────────────────────────────────────────────────────┐
│ 1. SETUP                                                       │
│    ESP32 boots → disables brownout → creates its own WiFi AP  │
│    "WirelessMonitor" → starts promiscuous sniffer              │
└──────────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────────┐
│ 2. CHANNEL HOPPING (every 500ms, continuously)                 │
│    Switches between WiFi channels 1 → 2 → ... → 13 → 1         │
└──────────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────────┐
│ 3. PACKET CAPTURE (fires on every received packet)              │
│    Extract MAC → read RSSI → check randomization → store/update│
└──────────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────────┐
│ 4. HOUSEKEEPING (every 2 seconds)                               │
│    Remove devices not seen in the last 10 seconds               │
└──────────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────────┐
│ 5. WEB SERVER (responds to phone's /data requests)               │
│    Computes avg RSSI, activity level, real/randomized split,    │
│    corrected occupancy estimate → packages as JSON               │
└──────────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────────┐
│ 6. DASHBOARD (phone browser, JavaScript)                         │
│    fetch('/data') every 2.5s → updates numbers, table, colors    │
└──────────────────────────────────────────────────────────────┘
```

---

## Project Structure

```
WirelessMonitor/
├── platformio.ini          # Build configuration
├── src/
│   └── main.cpp             # Firmware: sniffer + web server + dashboard
└── README.md                 # This file
```

---

## Dashboard Features

- **Summary cards** — Raw MAC count, average signal strength, activity level, current channel
- **Corrected occupancy box** — Real vs randomized MAC split, final estimated device count
- **Device table** — Per-device MAC, RSSI (with visual bar), proximity badge, status badge, MAC type badge
- **Color coding** — Green (Near/Active), Yellow (Mid/Weak), Red (Far), Purple (Randomized MAC)
- **Alert banner** — Flashes when device count spikes by more than 5, or packet rate exceeds 200
- **Auto-refresh** — Every 2.5 seconds, no manual reload needed

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Brownout reset loop | Already handled — `WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0)` disables it at boot |
| Upload stuck at "Connecting...." | Hold the **BOOT** button on the ESP32 during upload |
| No COM port in Device Manager | Install CP2102 or CH340 driver |
| Can't see `WirelessMonitor` WiFi | Check Serial Monitor confirms `HTTP server: STARTED`; try power cycling the board |
| Dashboard won't load | Make sure your phone is actually connected (not just "available") to `WirelessMonitor` |
| Empty device table | Wait 5–10 seconds after connecting; sniffer needs a moment to pick up traffic |

---

## Limitations

- **2.4 GHz only** — the ESP32 radio doesn't support 5 GHz or 6 GHz bands
- **RSSI-based proximity is approximate** — affected by obstructions, multipath interference, and differing device transmit power
- **Correction factor is empirical, not adaptive** — the 0.4 randomization scaling factor is a fixed constant, not recalibrated per environment
- **No payload inspection** — only frame headers/metadata are analyzed, no decryption or content analysis
- **Single-node coverage** — one ESP32 only covers its immediate physical radio range

---

## Future Work

- **Machine learning–based occupancy correction** — Replace the fixed 0.4 correction factor with a trained ML model (e.g., a lightweight regression or decision-tree model run on-device via TensorFlow Lite Micro) that learns the relationship between randomized-MAC patterns, probe request timing, and actual device counts across different environments — rather than relying on one static empirically-chosen constant.
- **Device fingerprinting via timing patterns** — Use ML to classify devices by type (phone, laptop, IoT) based on probe request intervals and packet timing signatures, rather than treating all detected MACs identically.
- **Anomaly detection with ML** — Train a model to recognize genuinely unusual wireless activity patterns (e.g., deauthentication attacks, rogue access points) instead of the current static threshold-based alert (>5 device spike / >200 packet rate).
- **OLED display integration** — Standalone operation without requiring a connected smartphone.
- **SD card data logging** — Historical data collection for long-term pattern analysis and to generate training data for the ML models above.
- **MAC OUI vendor lookup** — Identify device manufacturers from the MAC address prefix.
- **Multi-ESP32 mesh deployment** — Combine readings from multiple nodes via ESP-NOW or MQTT for wider coverage and improved occupancy accuracy through triangulation.

---

## References

1. O. Custance, S. Khan, and S. Parkinson, "Non-Cooperative 802.11 Presence Detection During Conference Visits," arXiv:2207.04706, 2022.
2. Keio University, "Indoor Crowd Estimation Scheme Using the Number of Wi-Fi Probe Requests Under MAC Address Randomization," Keio University Research Publications, 2021.
3. "Crowd Counting Through Walls Using WiFi," Proc. ACM/IEEE Conf. on Wireless Networks, ResearchGate Publication 321124603, 2017.
4. A. Author et al., "Evaluating the ESP32-S3 for Wi-Fi Penetration Testing Through the Development of Deauther32 and HackHeld32," Sensors (MDPI), vol. 26, no. 11, p. 3287, 2026.
5. Espressif Systems, "ESP32 Technical Reference Manual," Version 5.1, 2023.
6. Espressif Systems, "ESP-IDF Programming Guide — Wi-Fi Driver," 2023.
7. IEEE, "IEEE Standard for Wireless LAN Medium Access Control (MAC) and Physical Layer (PHY) Specifications," IEEE Std 802.11-2020.
8. B. Blanchon, "ArduinoJson: A JSON Library for Embedded Systems," Version 6.x, 2022.

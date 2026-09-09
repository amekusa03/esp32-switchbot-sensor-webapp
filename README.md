[日本語版のREADMEはこちら (Japanese README is available here)](./README.JP.md)

---

# ESP32-C3 SwitchBot Smart Home & smartOptSensor / Temp Hub

An integrated smart home hub firmware running on the **ESP32-C3**. It combines a high-performance embedded Web server for controlling devices via **SwitchBot Open API v1.1**, real-time illuminance and temperature monitoring (CdS photoresistor + B3950 NTC thermistor), and a **Matter Over Wi-Fi Light Sensor** endpoint running concurrently.

---
![SwitchBot Web UI](./web_page.png)

## 🌟 Key Features

- **ESP32-C3 Optimized**: Tailored firmware configuration designed for ESP32-C3 RISC-V SoC.
- **B3950 10k Thermistor Temperature Monitoring**:
  - **Measurement**: Real-time temperature measurement (°C) via `GPIO3` (ADC1_CH3) using a B3950 10k NTC thermistor with a 10kΩ pull-down resistor circuit and Steinhart-Hart / B-parameter (3950K) equation.
  - **Web UI Calibration / Offset**: Configurable ±°C temperature offset directly from the calibration panel, persisted in NVS across reboots.
- **smartOptSensor Illuminance (Lux) Measurement**:
  - **Pin Mapping**: `GPIO1` (ADC1_CH1) samples voltage over a CdS photoresistor and pull-down resistor (~9.1kΩ, 100kΩ || 10kΩ parallel) with high-precision multisampling (10-sample distributed averaging every 1,000ms).
  - **Lux & Matter Conversion**: Converts calibrated ADC voltage to CdS resistance, Lux, and Matter standard `MeasuredValue` (`10000 * log10(lux) + 1`) in real-time for both serial log and Web UI.
  - **Web UI 2-Point Calibration**: Interactive 2-point live calibration directly on the Web dashboard. Parameters (`A`, `R_ref`, `n`) are stored persistently in NVS.
- **Matter Over Wi-Fi Support**: Provides a native Matter Light Sensor Endpoint (`0x0106`, Cluster `0x0400`), allowing seamless integration into smart home ecosystems like Google Home and Apple HomeKit as an illuminance sensor.
- **Factory Reset**: Long press the `GPIO9` button for >3 seconds to trigger Matter commissioning and NVS configuration resets.
- **SwitchBot Open API v1.1 HMAC-SHA256 Authentication**: Built-in cryptographic signature generation using `mbedtls` with `token`, `secret`, millisecond timestamp `t`, and random `nonce`.
- **NTP Time Synchronization**: Automated time synchronization with standard NTP servers via `esp_netif_sntp` for accurate cryptographic API request signing.
- **Responsive Glassmorphism Web UI**: Unified web dashboard featuring live sensor monitoring banners, dynamic SwitchBot appliance control cards (Air Conditioner, TV, Bots, Plugs, Lights, Scenes), and modal configuration.
- **mDNS (Bonjour) Support**: Accessible via `http://esp32-switchbot.local/` without needing to remember IP addresses. Uses delegate hostname scheme compatible with Matter mDNS.
- **SoftAP Fallback & NVS Storage**: Automatically launches AP mode (`ESP32-SwitchBot-Setup`, `192.168.4.1`) when Wi-Fi credentials are not set or connection fails.

---

## 🔌 Pin Mapping

| ESP32-C3 Pin | Connected To | Role |
| :--- | :--- | :--- |
| **3.3V** | Power rail of CdS photoresistor & Thermistor | Power Supply |
| **GPIO1 (ADC1_CH1)** | Node between CdS cell and pull-down resistor (~9.1kΩ) | ADC Illuminance Measurement |
| **GPIO3 (ADC1_CH3)** | Node between B3950 Thermistor and pull-down resistor (10kΩ) | ADC Temperature Measurement |
| **GND** | Ground rail of pull-down resistors | Ground |
| **GPIO9** | Tactile push button (pulled to GND when pressed) | Factory Reset (Hold for >3s) |

---

## 🛠 Build & Flash

### 1. Load ESP-IDF & ESP-Matter Environment

```bash
source /home/kusa/esp/esp-idf/export.sh
source /home/kusa/esp/esp-matter/export.sh
```

### 2. Set Target to ESP32-C3

```bash
cd /path/to/SwitchBot+sensor
idf.py set-target esp32c3
```

### 3. Build the Project

```bash
idf.py build
```

### 4. Flash to ESP32-C3 and Monitor Serial Output

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

---

## 🌐 Network Access

Once connected to your local Wi-Fi, you can access the dashboard using any of the following URLs:

| Method | URL | Description |
| :--- | :--- | :--- |
| **mDNS (Recommended)** | `http://esp32-switchbot.local/` | Supported on iOS, macOS, Windows 10+, and Linux (Avahi) |
| **Direct IP Address** | `http://<ESP32_IP_ADDRESS>/` | Check serial monitor output for assigned IP |
| **SoftAP Mode** | `http://192.168.4.1/` | Initial configuration or fallback mode |

> **💡 Note for Linux users**: Install `avahi-daemon` and `libnss-mdns` to resolve `.local` domains:
> ```bash
> sudo apt install avahi-daemon libnss-mdns
> ```

---

## 📡 REST API Specifications (Local Endpoints)

| Endpoint | Method | Description |
| --- | --- | --- |
| `/` | `GET` | Serves the single-page responsive Web Dashboard |
| `/api/status` | `GET` | Retrieves system status (IP address, Wi-Fi RSSI, free heap, NTP sync state) |
| `/api/sensor` | `GET` | Retrieves live sensor data (`raw_adc`, `lux`, `temp_celsius`, `temp_offset`, etc.) |
| `/api/config` | `POST` | Saves Wi-Fi and SwitchBot API credentials into NVS |
| `/api/devices` | `GET` | Fetches device list and IR remotes from SwitchBot Cloud API |
| `/api/device/command` | `POST` | Sends control command to a SwitchBot device (`{"deviceId":"...","command":"turnOn"}`) |
| `/api/scenes` | `GET` | Fetches registered SwitchBot automation scenes |
| `/api/scene/execute` | `POST` | Executes a specific scene (`{"sceneId":"..."}`) |
| `/api/sensor/calibrate` | `GET` | Retrieves current CdS calibration parameters (`raw1`, `lux1`, `raw2`, `lux2`, `A`, `R_ref`, `n`) |
| `/api/sensor/calibrate` | `POST` | Records calibration points, saves temperature offset (`{"action":"set_temp_offset","offset":-1.5}`), or persists to NVS |

---

## ⚙️ Initial Setup Guide

### 1. Wi-Fi & SwitchBot API Configuration
1. Power on the ESP32-C3. If unconfigured, connect to the Wi-Fi AP named `ESP32-SwitchBot-Setup` and open `http://192.168.4.1/`.
2. Click **⚙️ Settings** in the top right corner.
3. Enter your **Wi-Fi SSID & Password**.
4. Retrieve your **Open API Token** and **Secret Key** from the SwitchBot mobile app:
   - Open SwitchBot App > Profile > Developer Options > Enable Developer Options.
   - Copy the Token and Secret Key.
5. Paste them into the dialog and click **Save & Connect**.

### 2. Matter Commissioning (Google Home / Apple Home)
1. Ensure the ESP32-C3 is connected to your local network.
2. In your Google Home or Apple Home app, tap **Add Device** > **Matter-enabled device**.
3. Commission using the Matter QR Code or manual pairing code output on the ESP32 serial monitor during initial boot.

### 3. Sensor Calibration (Web Dashboard)
1. Click **🔧 Calibration ▼** on the ambient sensor banner.
2. Under **Point 1 (Bright)**, measure the ambient light level with a lux meter or reference app, input the value, and click **Record Current**.
3. Cover the sensor or darken the room, input the low lux value under **Point 2 (Dark)**, and click **Record Current**.
4. For temperature, if there is a discrepancy with your reference thermometer, input the temperature offset (e.g. `-1.5` or `+0.8`) and click **Save Temp Offset**.
5. Click **💾 Save to NVS** to store the calibration curves into non-volatile flash storage.

---

## 📜 License

This project is open source and available under the [MIT License](LICENSE).

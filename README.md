# 📟 ManzLiteOS v7.0 — ESP32 + 16x2 LCD Embedded Operating System

A lightweight, feature-packed embedded operating system developed in C++ for the **ESP32** microcontroller paired with an **HD44780 16x2 I2C character LCD**. 

ManzLiteOS bridges your physical workspace with your phone, serving as a glanceable desktop notification monitor, BLE device scanner, and music telemetry display.

---

## ⚡ Key Capabilities

- **🍎 Native Apple iOS ANCS Client**: Connects to an iPhone over Bluetooth Low Energy (BLE) using Apple Notification Center Service. Displays incoming phone calls, SMS messages, and app notifications directly on the 16x2 LCD with zero companion app needed on iOS.
- **🤖 Android Tasker / BLE GATT Bridge**: Hosts a custom BLE GATT service with dedicated notification and media characteristics. Supports pipe-separated message formatting from Tasker / AutoNotification:
  - Notifications: `TYPE|SENDER|MESSAGE` (e.g. `SMS|Mom|Hey are you home?`, `CALL|Dad|Incoming call`)
  - Music HUD: `MUSIC|ARTIST|SONG TITLE` (e.g. `MUSIC|The Weeknd|Blinding Lights`)
- **📡 BLE Device Scanner**: Embedded Bluetooth scanner using standard ESP32 `BLEDevice` and `BLEAdvertisedDevice` APIs to discover nearby BLE beacons, wearables, and devices with signal strengths (RSSI).
- **🛡️ Emoji & Control Character Sanitizer**: Custom string filter that strips unprintable control codes and Unicode emojis before streaming text to the HD44780 controller, eliminating LCD driver lockups.
- **💤 Sleep & Power Management**: Built-in inactivity timers and sleep state routines.

---

## 🛠️ Hardware Requirements & Pinout

| Component | ESP32 GPIO Pin | Details |
| :--- | :--- | :--- |
| **I2C LCD SDA** | `GPIO 21` | Connect to 16x2 I2C backpack SDA |
| **I2C LCD SCL** | `GPIO 22` | Connect to 16x2 I2C backpack SCL |
| **LCD Power** | `5V / VIN` & `GND` | I2C LCD backpack power |
| **Buttons** | Configurable | Navigation buttons for menu control |

---

## 🚀 Building & Flashing

### Using PlatformIO (Recommended)
1. Install [PlatformIO](https://platformio.org/) in VS Code or CLI.
2. Clone this repository:
   ```bash
   git clone https://github.com/SuperManzDev/ManzLiteOS.git
   cd ManzLiteOS
   ```
3. Connect your ESP32 via USB and flash:
   ```bash
   pio run --target upload
   ```
4. Open the serial monitor at 115200 baud:
   ```bash
   pio device monitor
   ```

### Using Arduino IDE
1. Install the **ESP32 board package** via the Boards Manager.
2. Install dependencies via Library Manager:
   - `LiquidCrystal_I2C`
   - `ArduinoJson`
   - `ESP32-BLE-ANCS-Notifications`
3. Open [`src/main.cpp`](src/main.cpp), select your ESP32 board, and upload.

---

## 📱 Pairing Instructions

### iOS (iPhone)
1. Boot the ESP32 and select **Phone Notify** from the main LCD menu.
2. On your iPhone, open **Settings > Bluetooth**.
3. Locate the ESP32 device and tap **Pair**.
4. Confirm "Show Notifications" is enabled in the Bluetooth device info on iOS.

### Android (Tasker)
1. Pair your Android phone with the ESP32.
2. Configure AutoNotification / Tasker to write pipe-formatted messages (`TYPE|SENDER|MESSAGE`) to GATT characteristic `beb5483e-36e1-4688-b7f5-ea07361b26a8`.

---

## 📜 License
MIT License. Created by [Manz](https://github.com/SuperManzDev).

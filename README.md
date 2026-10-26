# OpenRFID Scanner (ESP32 + PN5180)

[![Tests](https://img.shields.io/badge/native--tests-48%2F48%20passed-brightgreen.svg)]()
[![Platform](https://img.shields.io/badge/platform-ESP32%20DevKit-blue.svg)]()
[![NFC](https://img.shields.io/badge/NFC-PN5180%20(ISO%2014443--A%20%2F%20NTAG215)-orange.svg)]()
[![Web UI](https://img.shields.io/badge/Web%20Config-Web%20Bluetooth-brightgreen.svg)](https://enohriel.github.io/openrfid_scanner/)

An intelligent, autonomous NFC spool scanner and tracking device powered by an **ESP32** and an **NXP PN5180** module. Designed specifically for **Prusa 3D printers (MK4S / MK4 / XL / MINI)** running **PrusaLink** and integrated with **Spoolman**.

> **Note on this Fork:**  
> This project is a comprehensive evolution of [ryanch/openprinttag_scanner](https://github.com/ryanch/openprinttag_scanner). While the original project was designed strictly around ISO 15693 (I-Code SLIX) tags and manual re-tagging, this fork re-architects the firmware for **ISO 14443-A (NTAG215)** tags, interoperability with the **OpenSpool / Snapmaker U1 ecosystem**, and **100% automated filament deduction in Spoolman**.

---

## 🌟 Key Features & Improvements over Upstream

### 1. Native ISO 14443-A & NTAG215 Support (PN5180)
- **Beyond ISO 15693:** Replaces the single-standard driver with a custom, low-level ISO 14443-A driver (`PN5180ISO14443`) for the PN5180.
- **Full NTAG215 Support:** Works natively with 7-byte UID tags standard across 3D printing (Snapmaker U1, Sunlu, Polymaker, recycled Anycubic/Amiibo tags).
- **4x Faster NFC Reads:** Implements 16-byte block reads (`readBlock`, command `0x30`) across 19 transactions instead of 76 individual 4-byte page reads. Read time dropped from ~800 ms to **~180 ms**, ensuring flawless scans on quick spool taps.
- **RX CRC handling:** Custom 4-bit ACK (`0x0A`) handling without hardware CRC framing errors during tag writing.

### 2. Dual Standard Parser & Snapmaker U1 Interoperability
- **NDEF OpenSpool Parser (`OpenSpoolParser`):** Native JSON MIME `application/json` serialization and deserialization, auto-generating Capability Containers (CC page 3) and NDEF TLVs.
- **Cross-Platform Spoolman Tracking:** Queries Spoolman seamlessly across multiple UUID fields:
  - `extra.openrfid_uuid`
  - `extra.openspool_uuid`
  - `extra.openprinttag_uuid`
  - `extra.card_uids` (including comma-separated lists for dual-sided spools).
- **Streaming JSON (`htcw_json`):** Non-blocking stream parsing preventing memory fragmentation or stack overflows when browsing large spool inventories.

### 3. Zero-Touch Automatic Deduction in Spoolman
- **No Re-Tagging Required:** In upstream, the user had to scan the tag again after printing to update weight. This fork automatically calls the native Spoolman API (`PUT /api/v1/spool/{id}/use`) upon print completion (`finishPrint`), deducting weight, updating filament length, and updating `last_used` timestamps autonomously.
- **Active Spool Memory:** Retains the assigned spool through pre-print scans and print starts, even after the physical spool has been lifted from the antenna and mounted on the printer holder.
- **Fast PrusaLink Metadata Extraction:** Interrogates PrusaLink `/api/v1/files/{storage}/{path}` for pre-calculated slicer metadata in ~50 ms. Resilient streaming fallback supports OrcaSlicer, PrusaSlicer, Bambu Studio, Cura, and binary `.bgcode` files.

### 4. Hardened BLE Configuration & Web App
- **Web Bluetooth Interface:** Accessible directly in Chromium browsers at **[https://enohriel.github.io/openrfid_scanner/](https://enohriel.github.io/openrfid_scanner/)** (or via local `docs/index.html`).
- **SEC-01 Secret Protection:** Sensitive credentials (`prusa_link_api_key`, `wifi_pass`, `ha_mqtt_pass`) are **never** transmitted in plain text over BLE (exposed as boolean flags like `prusa_link_api_key_set: true`).
- **Application PIN Authentication:** Changes require a 6-digit PIN (default: `123456`, customizable via UI).
- **Partial Updates & Auto-Reload:** Saving settings only transmits modified fields, avoiding accidental erasure of Wi-Fi or server credentials. "Reload Config" button and auto-load on tab navigation.

### 5. Clean, Flicker-Free LCD Interface (16x2 I2C)
- **State Machine Isolation (`LCDDisplayLogic`):** Prevents carousel loops and flickering between transient screens (`"Syncing Spoolman"`) and final status screens.
- **Clear Status Screens:** Displays spool details (Brand, Material, Remaining Weight) on scan, and locks on:
  ```text
  Done: -12.4g
  Remain: 840g
  ```
  once print deduction is confirmed by Spoolman.
- **Boot Splash:** Briefly displays the BLE PIN and assigned IP address on startup.

### 6. Embedded Test Tools & Native Unit Testing
- **Serial Print Simulator (`sim <grams>`):** Test the complete print-finish-deduct workflow live via serial without heating or printing (e.g. `sim 15.5`).
- **48 Native Unit Tests:** Comprehensive suite covering state flows, NFC parsing, raw page writes, bgcode parsing, and display logic running natively without hardware (`mingw32-make -C test/native`).

---

## 🔌 Hardware Setup

### Required Components
| Component | Specification | Quantity |
| :--- | :--- | :---: |
| **Microcontroller** | ESP32 DevKitC V4 (ESP32-WROOM-32) | 1 |
| **NFC Transceiver** | NXP PN5180 NFC Module | 1 |
| **Display** | 16x2 I2C LCD Display (HD44780 + PCF8574, 5V) | 1 |
| **Cabling** | Female-to-Female Dupont wires + 5V Splitter wire | 1 set |
| **Power** | USB-A to USB-C cable (or Micro-USB depending on DevKit) | 1 |

---

### Wiring Guide

#### 1. PN5180 NFC Module <-> ESP32 (SPI)
| PN5180 Pin | ESP32 Pin | DevKit Label | Function / Notes |
| :--- | :--- | :--- | :--- |
| **+5V (VIN)** | 5V / VIN | Top-left 5V pin | High-power RF carrier supply (shared with LCD via splitter) |
| **+3.3V** | 3V3 | 3V3 | Digital logic supply |
| **GND** | GND | GND | Common ground (*verify physical ground; do not connect to P12*) |
| **RST** | GPIO 13 | D13 | Hardware reset (active low) |
| **NSS** | GPIO 14 | D14 | SPI Chip Select (active low) |
| **MOSI** | GPIO 27 | D27 | SPI Master Out Slave In |
| **MISO** | GPIO 26 | D26 | SPI Master In Slave Out |
| **SCK** | GPIO 25 | D25 | SPI Clock |
| **BUSY** | GPIO 33 | D33 | PN5180 busy flow control |

*(Note: PN5180 pins GPIO, IRQ, AUX, and REQ are left unconnected for ISO 14443-A operation).*

#### 2. LCD 16x2 Display <-> ESP32 (I2C)
| LCD I2C Pin | ESP32 Pin | Function / Notes |
| :--- | :--- | :--- |
| **VCC** | 5V / VIN | 5V supply (shared via splitter) |
| **GND** | GND | Common ground |
| **SDA** | GPIO 23 (D23) | I2C Data |
| **SCL** | GPIO 22 (D22) | I2C Clock |

---

## ⚙️ Configuration & First Run

1. **Power up the device** via USB. The LCD will display initialization messages and the default BLE PIN (`123456`).
2. **Open the Web Configuration Tool:**
   - Navigate to **[https://enohriel.github.io/openrfid_scanner/](https://enohriel.github.io/openrfid_scanner/)** in Chrome, Edge, or any Web Bluetooth compatible browser.
3. **Connect to the device:**
   - Click **Connect** and select `OpenPrintTag` / `OpenRFID` from the Bluetooth prompt.
4. **Configure Settings:**
   - Enter your current PIN (default: `123456`).
   - Configure **Wi-Fi** SSID and Password.
   - Configure **PrusaLink** IP address and API Key.
   - Configure **Spoolman** IP address and Port (e.g. `192.168.1.50`, port `7912`).
   - (Optional) Set a new 6-digit Bluetooth PIN.
5. Click **Save Configuration**. The scanner will store your preferences in NVS, connect to your Wi-Fi, and sync with your printer and Spoolman.

---

## 🛠️ Development & Testing

### Running Native Unit Tests
All business logic, parsers, and state transitions are decoupled from Arduino hardware and tested with MinGW / GCC:
```bash
# Build and run all 48 native tests
mingw32-make -C test/native
```

### Compiling & Flashing the Firmware
Using [PlatformIO](https://platformio.org/):
```bash
# Compile firmware
pio run

# Flash to ESP32 over USB
pio run -t upload

# Open serial monitor
pio device monitor -b 115200
```

### Live Serial Simulation
You can trigger a full simulated print cycle directly via the serial terminal:
```text
sim 12.5
```
This simulates a 12.5g print job starting and finishing, demonstrating immediate Spoolman deduction and LCD update.

---

## 📄 License & Acknowledgments

- Original project by **[ryanch](https://github.com/ryanch/openprinttag_scanner)** under MIT License.
- PN5180 library based on work by **ATrappmann** and **Tueddy**, enhanced for ISO 14443-A block reading.
- Standard specifications: [OpenSpool](https://github.com/open-spool/openspool-standard) & [OpenPrintTag](https://openprinttag.org).

# Handoff - OpenRFID / OpenSpool Scanner (ESP32 + PN5180)

**Date :** 26 Octobre 2026  
**Objectif :** Scanner NFC pour **Prusa MK4S** et **Spoolman**, compatible 100% avec les tags **NTAG215 (OpenSpool / OpenRFID)** partagés avec la Snapmaker U1.

---

## 1. État Actuel du Projet
- **Adaptation code terminée et testée à 100% :** 39/39 tests natifs passent (NFC Read, Raw Write, App Flow, LCD Manager, Bgcode Parser).
- **Driver NFC :** PN5180 configuré en ISO 14443-A (`PN5180ISO14443`), gestion des UID 7 octets et commandes NTAG215 (0x30 lecture bloc 16 octets, 0xA2 écriture page 4 octets).
- **Parser NDEF OpenSpool :** `OpenSpoolParser` gère la sérialisation/désérialisation du JSON MIME `application/json` et la création automatique du Capability Container en page 3.
- **Interopérabilité Spoolman :** Support simultané de `openrfid_uuid`, `openspool_uuid` et `openprinttag_uuid` pour reconnaissance mutuelle avec la Snapmaker U1.

---

## 2. Guide de Câblage Matériel

### A. Module NFC PN5180 <-> ESP32 DevKit (SPI)
| PN5180 | ESP32 (GPIO) | Remarque |
| :--- | :--- | :--- |
| **VCC** | **3.3V** | Ne pas alimenter en 5V (logique 3.3V) |
| **GND** | **GND** | Masse commune |
| **MISO** | **GPIO 19** | SPI Master In Slave Out |
| **MOSI** | **GPIO 23** | SPI Master Out Slave In |
| **SCK** | **GPIO 18** | SPI Clock |
| **NSS** *(ou CS)* | **GPIO 5** | SPI Chip Select |
| **BUSY** | **GPIO 16** | Signal Busy PN5180 |
| **RST** | **GPIO 17** | Hardware Reset |
| **REQ** | *Non connecté* | Inutilisé en liaison SPI standard |

### B. Écran LCD 16x2 I2C <-> ESP32
| LCD I2C | ESP32 (GPIO) | Remarque |
| :--- | :--- | :--- |
| **VCC** | **5V / VIN** | La plupart des afficheurs 16x2 demandent 5V |
| **GND** | **GND** | Masse commune |
| **SDA** | **GPIO 21** | I2C Data |
| **SCL** | **GPIO 22** | I2C Clock |

---

## 3. Procédure pour Demain (Assemblage & Flash)

1. **Assembler les connexions** sur breadboard ou avec des câbles Dupont selon le tableau ci-dessus.
2. **Brancher l'ESP32** en USB sur votre PC.
3. **Téléverser le firmware** via PlatformIO :
   - Depuis VS Code avec l'extension PlatformIO : ouvrir le dossier `openrfid_scanner` et cliquer sur **Upload**, OU
   - En ligne de commande : `pio run -t upload`
4. **Configuration initiale (via Bluetooth / Web Bluetooth)** :
   - À l'allumage, l'ESP32 diffuse un point d'accès BLE `OpenPrintTag-XXXX`.
   - Ouvrir la page de configuration ou connecter le scanner au réseau Wi-Fi.
   - Configurer :
     - **Wi-Fi SSID & Mot de passe**
     - **PrusaLink URL :** `http://192.168.1.74/` + Clé API de la MK4S
     - **Spoolman URL :** `http://192.168.1.161:7912`
5. **Test avec une bobine** :
   - Approcher une bobine NTAG215 (déjà utilisée sur la Snapmaker U1).
   - L'écran LCD affiche immédiatement la matière (ex. `PETG`), la marque (ex. `Prusament`) et le poids restant.
   - Spoolman sélectionne automatiquement la bobine correspondante.

---

## 4. Fichiers Modifiés & Créés
- Driver : `lib/PN5180/PN5180ISO14443.h`, `lib/PN5180/PN5180ISO14443.cpp`
- Connexion matérielle : `src/HardwareNFCConnection.h`, `src/HardwareNFCConnection.cpp`, `src/NFCConnectionI.h`
- Parser : `src/OpenSpoolParser.h`, `src/OpenSpoolParser.cpp`, `src/NFCTypes.h`
- Gestionnaire NFC : `src/NFCManager.h`, `src/NFCManager.cpp`
- Spoolman : `src/SpoolmanManager.cpp`
- Utilitaires : `src/ConversionUtils.cpp`
- Tests : `test/native/StubNFCConnection.h`, `test/native/test_nfc_read.cpp`, `test/native/test_raw_write.cpp`, `test/native/Makefile`

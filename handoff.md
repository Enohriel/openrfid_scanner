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

## 2. Guide de Câblage Matériel (Validé et Opérationnel)

### A. Module NFC PN5180 <-> ESP32 DevKit (SPI)
| PN5180 | ESP32 | Emplacement DevKit | Remarque |
| :--- | :--- | :--- | :--- |
| **+5V** | **5V / VIN** | Coin supérieur gauche | Alimentation émetteur RF (partagé avec LCD via câble splitter) |
| **+3.3V** | **3V3** | Broche 3V3 | Logique numérique PN5180 |
| **GND** | **GND** | Broche GND | **Important :** Masse réelle (ne jamais brancher sur P12) |
| **RST** | **GPIO 13** | D13 | Hardware Reset (actif bas) |
| **NSS** | **GPIO 14** | D14 | SPI Chip Select |
| **MOSI** | **GPIO 27** | D27 | SPI Master Out Slave In |
| **MISO** | **GPIO 26** | D26 | SPI Master In Slave Out |
| **SCK** | **GPIO 25** | D25 | SPI Clock |
| **BUSY** | **GPIO 33** | D33 | Signal Busy PN5180 |

### B. Écran LCD 16x2 I2C <-> ESP32
| LCD I2C | ESP32 | Emplacement DevKit | Remarque |
| :--- | :--- | :--- | :--- |
| **VCC** | **5V / VIN** | Coin supérieur gauche | Partagé via le câble splitter 5V |
| **GND** | **GND** | Broche GND | Masse commune |
| **SDA** | **GPIO 23** | D23 | I2C Data |
| **SCL** | **GPIO 22** | D22 | I2C Clock |

---

## 3. Statut de Validation
- **Scan NFC NTAG215 (ISO 14443-A) :** Validé en conditions réelles avec `UID 04C2F379CD2A81` (Sunlu) et `048C3157CE2A81` (Polymaker).
- **Lecture optimisée :** Lecture en blocs de 16 octets (`readBlock`) avec retries sur 19 transactions au lieu de 76 (temps de lecture divisé par 4, ~180 ms), éliminant les échecs lors des passages rapides.
- **Parsing OpenSpool :** Extraction complète des métadonnées (Marque, Type, Couleur, Poids, ID).
- **Interopérabilité Spoolman & Spool Painter :**
  - Support natif du champ `extra.card_uids` (incluant les listes multi-UID séparées par des virgules pour bobines avec plusieurs tags/faces, ex: `"53BD9CFE420001,048C3157CE2A81"`).
  - Correction du streaming JSON `htcw_json` dans `SpoolmanManager.cpp` (`parseSpoolIdByUuid`) pour les tags non formatés OpenSpool (tags Anycubic recyclés) : gestion correcte de la profondeur `reader.depth() < extraDepth` et ciblage strict de `spool.id` sans collision avec `vendor.id`.
- **Cycle d'impression, Déduction de filament & Affichage LCD :**
  - Mémorisation de la dernière bobine scannée (`lastScannedSpoolId`) pour conserver l'assignation de la bobine au démarrage de l'impression même si le tag n'est plus posé sur le lecteur.
  - Correction de la construction d'URL PrusaLink (`buildJobUrl` et `hostByName`) pour éliminer les doubles slashes (`//api/v1/job/...`) et les échecs DNS causés par un slash de fin.
  - Résolution immédiate des erreurs HTTP 404/405/204 sur les jobs terminés sans gel de 2 minutes.
  - Support universel des fichiers G-code texte (`.gcode` / `.GCO` OrcaSlicer, PrusaSlicer, Bambu Studio, Cura) et binaires (`.bgcode` / `.BGC`) :
    - Interrogation prioritaire de l'endpoint natif PrusaLink `/api/v1/files/{storage}/{path}` extrayant les métadonnées pré-calculées (`filament used [g]`) en ~50ms sans téléchargement lourd.
    - Détection des extensions FAT32 8.3 (`.BGC`, `.bgc`) sur clé USB Prusa pour envoyer `Range: bytes=0-16383` aux fichiers `.bgcode` au lieu de lire la fin du fichier.
    - Correction du streaming G-code par blocs de 8 Ko (`feedGcodeStreamChunk`) : suppression de la régression qui coupait la lecture après 50 lignes sur `G1`, priorité explicite aux clés en grammes (`total filament used [g]` > `filament used [g]` > `[cm3]`/`[mm]`), et arrêt immédiat sur `CONFIG_BLOCK_START`.
    - **Élimination du blocage TCP Delayed ACK (`250 ms`) entre Prusa MK4S (`nhttp`) et ESP32 (`lwIP`)** : le serveur HTTP embarqué de la Prusa MK4S ignore l'en-tête `Range:` sur les `.gcode` (`HTTP 200` depuis l'octet 0) et envoie des segments de `1024` octets (`< MSS`) en attendant un ACK TCP avant le segment suivant. L'appel immédiat à `tcp_fasttmr()` via `tcpip_try_callback` après chaque lecture multiplie le débit par ~8x (`145 Ko` lus et `2.09g` extraits sur `/usb/TESTFU~1.GCO` en `4,3 s` au lieu de plafonner à `4 Ko/s` et d'expirer à `102 Ko`).
    - Libération de `httpMutex_` pendant le streaming du fichier pour ne jamais bloquer la synchronisation Spoolman lors d'un scan NFC en début d'impression.
    - Correction de la gestion de `printer.state` (`FINISHED` / `CANCELED`) et de `lastProgressPercent` dans `PrinterManager` pour éviter un calcul à `0g` ("No filament used") sur les impressions courtes se terminant entre deux polls.
  - **Déduction Spoolman 100% Automatique & Enchaînement des Prints :**
    - Remplacement de l'obligation de re-scanner le tag NFC par un appel API natif Spoolman direct (`PUT /api/v1/spool/{id}/use`) dès la fin de l'impression (`finishPrint`).
  - **Support Bobines Anycubic ACE & Lookup Spoolman Zéro-Allocation :**
    - Décodage natif binaire Anycubic ACE (Magic `7B 00 65 00`, SKU, marque `AC` -> `Anycubic`, matériau, couleur ABGR, températures, poids).
    - Décodage streaming sans allocation heap (`arduino_buffered_stream` sur pile de 512 octets) de `/api/v1/spool` pour éliminer les plantages de heap de l'ESP32 sur bases volumineuses (> 40 bobines).
  - **Résilience BLE & Web Bluetooth (Windows) :**
    - Correction de l'intervalle de connexion BLE publicitaire (`setMaxPreferred(0x12)` au lieu du doublon `setMinPreferred`).
    - Web App (`docs/index.html`) durcie avec délai de stabilisation WinRT (400ms) et jusqu'à 3 tentatives automatiques pour éliminer l'erreur `GATT Server is disconnected. Cannot retrieve services`.
    - L'ESP32 déduit automatiquement le poids consommé (`use_weight`) dans Spoolman, recalcule la longueur consommée et met à jour `last_used`.
    - **Affichage LCD stable (sans carrousel parasite) :** Correction de `LCDDisplayLogic` pour interdire la fusion des écrans transitoires (`"Syncing"`, `"Present tag"`). L'écran n'alterne plus en boucle : dès la fin de la synchronisation Spoolman, il affiche de manière fixe :
      - Ligne 1 : `Done: -X.Xg`
      - Ligne 2 : `Remain: XXXg` (poids réel restant remonté par Spoolman)
    - La bobine active reste mémorisée indéfiniment en mémoire pour enchaîner les impressions successives sans jamais avoir à manipuler la bobine.
  - **Simulateur d'impression intégré (`sim [grams]`) :** Commande série permettant de tester instantanément le cycle complet sans chauffer ni imprimer (ex: commande `sim 2.5` ou script `scratch/simulate_print.py`).
- **Flash PlatformIO :** Téléversement USB direct sans débrancher aucun câble.
- **Démarrage NFC & Résilience en impression active :** Tâche `NFCScanTask` lancée immédiatement à la première seconde de boot dans `main.cpp` (avant tout polling réseau), et arrêt instantané du streaming G-code texte dès les premières commandes d'extrusion. Validé en conditions réelles à travers le boîtier avec synchronisation Spoolman instantanée.

---

## 4. Fichiers Modifiés & Créés
- Driver : `lib/PN5180/PN5180ISO14443.h`, `lib/PN5180/PN5180ISO14443.cpp` (désactivation CRC RX pour ACK NTAG215, pause EEPROM 6ms)
- Connexion matérielle : `src/HardwareNFCConnection.h`, `src/HardwareNFCConnection.cpp`, `src/NFCConnectionI.h`
- Parser : `src/OpenSpoolParser.h`, `src/OpenSpoolParser.cpp`, `src/NFCTypes.h`
- Gestionnaire NFC : `src/NFCManager.h`, `src/NFCManager.cpp`
- Cycle d'application : `src/ApplicationManager.h`, `src/ApplicationManager.cpp` (gestion cycle automatique Spoolman, conservation spool active)
- PrusaLink : `src/PrusaLinkAPIStrategy.h`, `src/PrusaLinkAPIStrategy.cpp` (extraction poids via /files API & streaming G-code sans coupure)
- Configuration : `src/ConfigurationManager.cpp`
- Spoolman : `src/SpoolmanManager.h`, `src/SpoolmanManager.cpp` (implémentation `httpPut`, `enqueueUseFilament`, `useFilament` via `/api/v1/spool/{id}/use`, et recherche globale en streaming zéro-allocation `arduino_buffered_stream` sur `/api/v1/spool` pour bases de données volumineuses)
- Utilitaires : `src/ConversionUtils.cpp`
- Script de simulation : `scratch/simulate_print.py`, `scratch/monitor_live.py`
- Tests : `test/native/StubNFCConnection.h`, `test/native/test_nfc_read.cpp`, `test/native/test_raw_write.cpp`, `test/native/Makefile`
- Bluetooth & Web Config : `src/BluetoothManager.cpp`, `src/ConfigurationManager.cpp`, `docs/index.html` (protection PIN applicatif 6 chiffres, masquage des secrets, rechargement automatique et mises à jour partielles)
- Support Anycubic ACE RFID : `src/OpenSpoolParser.h`, `src/OpenSpoolParser.cpp`, `test/native/test_nfc_read.cpp` (décodage natif binaire Magic `7B 00 65 00`, extraction marque `Anycubic`, type `PETG`, couleur ABGR, températures et création automatique Spoolman)
  - **Mise à Jour OTA Sans Fil (ArduinoOTA) :**
    - Partitionnement dual-bank 4 Mo (`app0` et `app1` de 1,93 Mo).
    - Validation du téléversement sans fil direct via PlatformIO (`pio run -e esp32-ender-ota -t upload` sur `192.168.1.71`) avec reboot automatique sur la banque inactive.

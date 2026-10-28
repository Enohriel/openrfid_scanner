# CONTEXT.md - OpenRFID / OpenSpool Scanner

Ce document définit le vocabulaire du domaine, les règles d'architecture et les invariants techniques du projet **OpenRFID Scanner** (fork de `openprinttag_scanner`).

---

## 1. Vocabulaire du Domaine

- **OpenSpool / OpenRFID** : Standard ouvert de formatage NDEF JSON (`application/json`) sur tags NFC pour bobines de filament 3D (poids initial, restant, matériau, couleur, marque, ID).
- **Anycubic ACE** : Format binaire propriétaire Anycubic Kobra 3 / ACE (Magic `7B 00 65 00` en page 4, SKU en pages 5-9, Marque `AC` en page 10, Matériau en page 15, Couleur ABGR en page 20, Températures en pages 24/29, Poids en page 31) automatiquement décodé par `OpenSpoolParser`.
- **OpenPrintTag** : Spécification NFC initiale (généralement sur puces ISO 15693 / I-Code SLIX).
- **NTAG215 (ISO 14443-A)** : Type de tag NFC haute capacité (504 octets utilisateur, 135 pages de 4 octets) massivement utilisé dans l'écosystème 3D (Snapmaker U1, bobines Sunlu/Polymaker, etc.).
- **PN5180** : Émetteur-récepteur NFC SPI d'NXP supportant à la fois ISO 14443-A et ISO 15693 avec une puissance RF supérieure aux modules PN532/RC522.
- **Spoolman** : Gestionnaire de stock de filaments auto-hébergé avec API REST.
- **PrusaLink** : API HTTP locale embarquée sur les imprimantes Prusa (MK4, MK4S, XL, MINI) — `printer_type = 0`.
- **Moonraker** : Serveur d'API HTTP/WebSocket pour Klipper (Mainsail / Fluidd, ex: Ender 3 V3 KE sur port `:7125`) — `printer_type = 1`.
- **Automation Mode** :
  - `SELF_DIRECTED` : Mode autonome où le scanner déduit le filament consommé directement dans Spoolman via `PUT /api/v1/spool/{id}/use` en fin d'impression sans action manuelle.
  - `CONTROLLED` : Mode piloté par Home Assistant ou un orchestrateur MQTT.

---

## 2. Invariants Techniques

1. **Protection des Secrets (SEC-01)** :
   - `prusa_link_api_key`, `wifi_pass` et `ha_mqtt_pass` ne sont **jamais** renvoyés en clair sur le canal Bluetooth Low Energy.
   - Les lectures de configuration renvoient des booléens d'état (`prusa_link_api_key_set: true`, etc.).
   - Toute modification de configuration (`write_config`) exige la fourniture du code PIN applicatif (défaut `123456`).
   - Mises à jour partielles : seuls les champs non vides fournis par le client écrasent les valeurs existantes.

2. **Lecture NFC Optimisée (ISO 14443-A)** :
   - Lecture par blocs de 16 octets (`readBlock` commande 0x30) plutôt que page par page (4 octets, 0x30/4) pour diviser le temps de transaction par 4 (~180 ms) et fiabiliser la détection en passage rapide.
   - Désactivation du CRC matériel en réception RX pour les réponses d'acquittement 4 bits (ACK `0x0A`).

3. **Synchronisation Spoolman & Résilience** :
   - La recherche globale de bobine dans Spoolman (`/api/v1/spool`) utilise un analyseur de flux (`arduino_buffered_stream` par blocs de 512 octets) directement connecté à `WiFiClient`, éliminant tout buffer de 30 Ko en RAM pour supporter des bases de données volumineuses sans fragmentation ni saturation de heap.
   - Interrogation dans l'ordre : `extra.openrfid_uuid`, `extra.openspool_uuid`, `extra.openprinttag_uuid`, et `extra.card_uids` (gestion des listes multi-UID séparées par des virgules pour bobines Snapmaker ou multi-faces).

4. **Support Multi-Imprimantes (`IPrinterLinkStrategy`)** :
   - `PrusaLinkAPIStrategy` (`printer_type = 0`, affichage LCD `PL+`) : interroge `/api/v1/status`, `/api/v1/job`, `/api/v1/files/...`, et en repli parse l'en-tête binaire `.bgcode`/`.BGC` ou streame le fichier `.gcode`/`.GCO` par blocs de 8 Ko (`feedGcodeStreamChunk`) jusqu'à `CONFIG_BLOCK_START` en acquittant immédiatement les segments TCP de 1024 octets de `nhttp` (`tcp_fasttmr` via `tcpip_try_callback`) sans bloquer `httpMutex_`.
   - `MoonrakerAPIStrategy` (`printer_type = 1`, affichage LCD `MR+`, port `:7125` par défaut) : interroge `/printer/objects/query?print_stats&virtual_sdcard`, convertit `print_stats.filament_used` (mm) en grammes selon la densité/diamètre de la bobine scannée (avec repli sur `/server/files/metadata`), et assigne la bobine active dans Mainsail/Fluidd via `POST /server/spoolman/spool_id` (conservée au retrait du tag).

5. **Stabilité de l'Affichage LCD (16x2 I2C)** :
   - `LCDDisplayLogic` interdit la fusion ou l'alternance cyclique d'écrans transitoires (`Syncing`, `Present tag`) avec des écrans permanents.
   - En fin d'impression, l'écran reste fixé sur `Done: -X.Xg` / `Remain: XXXg`.

6. **Bluetooth Low Energy & Web Bluetooth** :
   - `BluetoothManager` configure Bluedroid avec le préfixe de nom `OpenPrintTag-XXXX`.
   - Intervalles de connexion publicitaires : `setMinPreferred(0x06)` (7.5ms) et `setMaxPreferred(0x12)` (22.5ms).
   - L'interface Web Bluetooth (`docs/index.html`) applique un délai de stabilisation de 400ms après `gatt.connect()` et jusqu'à 3 tentatives de découverte de service pour s'adapter au cache WinRT sous Windows/Chromium sans drop GATT prématuré.

---

## 3. Câblage Matériel (ESP32 DevKitC V4)

| Périphérique | Broche Composant | Broche ESP32 | Notes |
| :--- | :--- | :--- | :--- |
| **PN5180 (SPI)** | +5V (VIN) | 5V / VIN | Partagé avec LCD via splitter |
| | +3.3V | 3V3 | Logique numérique |
| | GND | GND | Masse commune |
| | RST | GPIO 13 (D13) | Reset matériel actif bas |
| | NSS | GPIO 14 (D14) | SPI Chip Select |
| | MOSI | GPIO 27 (D27) | Master Out Slave In |
| | MISO | GPIO 26 (D26) | Master In Slave Out |
| | SCK | GPIO 25 (D25) | SPI Clock |
| | BUSY | GPIO 33 (D33) | Signal d'occupation PN5180 |
| **LCD 16x2 (I2C)** | VCC | 5V / VIN | Alimentation 5V |
| | GND | GND | Masse |
| | SDA | GPIO 23 (D23) | I2C Data |
| | SCL | GPIO 22 (D22) | I2C Clock |

---

## 4. Commandes Utiles

- **Tests unitaires natifs** : `mingw32-make -C test/native`
- **Compilation firmware** : `pio run`
- **Flash USB** : `pio run -t upload`
- **Interface Web de configuration** : `https://enohriel.github.io/openrfid_scanner/` (ou `docs/index.html`)

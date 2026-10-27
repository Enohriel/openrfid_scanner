#include <Wire.h>
#include <WiFi.h>
#include <time.h>

#include "ConfigurationManager.h"
#include "BluetoothManager.h"
#include "ApplicationManager.h"
#include "PrinterManager.h"
#include "NFCManager.h"
#include "PrusaLinkAPIStrategy.h"
#include "SpoolmanManager.h"
#include "HomeAssistantManager.h"
#include "StubPrinterLinkStrategy.h"
#include "LCDManager.h"

// Global HTTP mutex for serializing WiFi HTTP requests
SemaphoreHandle_t g_httpMutex = nullptr;

// LCD I2C pins
#define LCD_SDA 23
#define LCD_SCL 22

// LCD Manager
LCDManager lcdManager(0x27, 16, 2);

void initWiFi() {
  auto& config = ConfigurationManager::getInstance();

  if (strlen(config.getWiFiSSID()) == 0) {
    Serial.println("WiFi SSID not configured - skipping WiFi");
    lcdManager.updateScreen("WiFi: no SSID", "Configure via BLE");
    delay(2000);
    return;
  }

  Serial.print("Connecting to WiFi: ");
  Serial.println(config.getWiFiSSID());

  lcdManager.updateScreen("Connecting WiFi", "");

  WiFi.begin(config.getWiFiSSID(), config.getWiFiPassword());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("WiFi connected! IP: ");
    Serial.println(WiFi.localIP());

    lcdManager.updateScreen("WiFi OK", WiFi.localIP().toString().c_str());

    Serial.println("Setting up NTP...");
    configTime(0, 0, "pool.ntp.org");
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
      Serial.println("Failed to obtain time");
      lcdManager.updateScreen("NTP FAILED", "");
    } else {
      Serial.println("Time obtained");
      //lcdManager.updateScreen("NTP OK", "");
    }

    delay(2000);
  } else {
    Serial.println("");
    Serial.println("WiFi connection failed!");

    lcdManager.updateScreen("WiFi FAILED", "");
    delay(2000);
  }
}

void setup() {
  delay(1000);
  Serial.begin(9600);
  delay(1000);
  Serial.println("=== Starting setup ===");

  // Initialize I2C and scan for LCD
  Serial.println("Scanning I2C for LCD...");
  uint8_t foundAddr = 0;

  // Try configured pins (SDA=23, SCL=22)
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setTimeOut(50);
  for (uint8_t a = 0x20; a <= 0x3F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("Found I2C device on SDA=%d, SCL=%d at 0x%02X\n", LCD_SDA, LCD_SCL, a);
      foundAddr = a;
      break;
    }
  }

  // If not found, try standard ESP32 pins (SDA=21, SCL=22)
  if (foundAddr == 0 && LCD_SDA != 21) {
    Wire.begin(21, 22);
    for (uint8_t a = 0x20; a <= 0x3F; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        Serial.printf("Found I2C device on SDA=21, SCL=22 at 0x%02X\n", a);
        foundAddr = a;
        break;
      }
    }
  }

  if (foundAddr != 0) {
    Serial.printf("Configuring LCD with address 0x%02X\n", foundAddr);
    lcdManager.setAddress(foundAddr);
  } else {
    Serial.println("WARNING: No I2C LCD detected! Proceeding without LCD");
  }

  // Initialize LCD and start its task on core 0
  lcdManager.begin();
  lcdManager.startTask();
  lcdManager.updateScreen("Initializing...", "");
  Serial.println("LCD initialized");

  // Initialize ConfigurationManager FIRST (loads NVS)
  if (!ConfigurationManager::getInstance().begin()) {
    Serial.println("ConfigurationManager init failed - halting");
    lcdManager.updateScreen("Config FAILED", "");
    while (1) { delay(1000); }
  }
  lcdManager.setScreenTimeoutMs(ConfigurationManager::getInstance().getLcdTimeoutMs());

  // Initialize ApplicationManager (message queue) with LCD reference
  if (!ApplicationManager::getInstance().begin(&lcdManager)) {
    Serial.println("ApplicationManager init failed - halting");
    lcdManager.updateScreen("AppMgr FAILED", "");
    while (1) { delay(1000); }
  }


  // Initialize BluetoothManager BEFORE WiFi (they share the radio)
  lcdManager.updateScreen("Starting BLE...", "");
  if (!BluetoothManager::getInstance().begin()) {
    Serial.println("BluetoothManager init failed - continuing without BLE");
  } else {
    char pinLine[17];
    snprintf(pinLine, sizeof(pinLine), "PIN: %06u", ConfigurationManager::getInstance().getBlePin());
    lcdManager.updateScreen("Bluetooth Ready", pinLine);
    delay(1000);
  }

  // Connect to WiFi
  initWiFi();

  // Create global HTTP mutex for serializing HTTP requests
  g_httpMutex = xSemaphoreCreateMutex();
  if (g_httpMutex == nullptr) {
    Serial.println("Failed to create HTTP mutex - halting");
    lcdManager.updateScreen("Mutex FAILED", "");
    while (1) { delay(1000); }
  }

  // Initialize SpoolmanManager
  if (!SpoolmanManager::getInstance().begin(g_httpMutex)) {
    Serial.println("SpoolmanManager init failed - continuing without Spoolman");
  }

  // Initialize HomeAssistantManager
  if (!HomeAssistantManager::getInstance().begin()) {
    Serial.println("HomeAssistantManager init failed - continuing without HA");
  }

  // Load automation mode from config
  {
    uint8_t mode = ConfigurationManager::getInstance().getAutomationMode();
    ApplicationManager::getInstance().setAutomationMode(static_cast<AutomationMode>(mode));
    Serial.printf("Automation mode: %s\n",
                  mode == 0 ? "SELF_DIRECTED" : "CONTROLLED_BY_HA");
  }

  // Initialize and start NFCManager FIRST so scanning is immediately active
  if (!NFCManager::getInstance().begin()) {
    Serial.println("NFCManager init failed - halting");
    lcdManager.updateScreen("NFC FAILED", "");
    while (1) { delay(1000); }
  }
  NFCManager::getInstance().startScanTask();

  // Initialize and start PrinterManager with background polling
  static PrusaLinkAPIStrategy printerStrategy;
  printerStrategy.setHttpMutex(g_httpMutex);
  PrinterManager::getInstance().setStrategy(&printerStrategy);
  PrinterManager::getInstance().begin();
  PrinterManager::getInstance().startPollingTask();

  // Start SpoolmanManager task
  SpoolmanManager::getInstance().startTask();

  // Build status + HA startup from same config snapshot
  auto& config = ConfigurationManager::getInstance();

  // Start HomeAssistantManager task
  Serial.printf("Setup: HA config before startTask: enabled=%s host='%s' host_len=%u port=%u user_set=%s\n",
                config.getHAEnabled() ? "true" : "false",
                config.getHAMqttHost(),
                static_cast<unsigned>(strlen(config.getHAMqttHost())),
                static_cast<unsigned>(config.getHAMqttPort()),
                strlen(config.getHAMqttUser()) > 0 ? "true" : "false");
  HomeAssistantManager::getInstance().startTask();

  ApplicationManager::getInstance().showStatusOnLCD();

  Serial.println("=== Setup complete ===");
}

static void handleSerialCommands() {
  static char cmdBuf[64];
  static size_t cmdLen = 0;

  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      cmdBuf[cmdLen] = '\0';
      if (cmdLen > 0) {
        if (strncmp(cmdBuf, "sim", 3) == 0 || strncmp(cmdBuf, "simulate", 8) == 0) {
          float grams = 2.10f;
          char* p = strchr(cmdBuf, ' ');
          if (p != nullptr) {
            float val = atof(p + 1);
            if (val > 0.0f) grams = val;
          }

          auto& app = ApplicationManager::getInstance();
          const char* targetSpool = app.getLastScannedSpoolId();
          if (targetSpool[0] == '\0') {
            targetSpool = app.getStartingSpoolId();
          }

          Serial.printf("SIMULATOR: Starting print simulation (%.2fg)...\n", grams);
          if (targetSpool[0] == '\0') {
            Serial.println("SIMULATOR: WARNING - No spool has been scanned yet! Please scan a spool first.");
          } else {
            Serial.printf("SIMULATOR: Target spool: %s\n", targetSpool);
          }

          AppMessage startMsg;
          startMsg.type = AppMessageType::PRINT_STARTED;
          startMsg.payload.printStarted.job_id = 9999;
          app.sendMessage(startMsg);

          vTaskDelay(pdMS_TO_TICKS(1500));

          AppMessage endMsg;
          endMsg.type = AppMessageType::PRINT_ENDED;
          endMsg.payload.printEnded.job_id = 9999;
          endMsg.payload.printEnded.filament_used_grams = grams;
          endMsg.payload.printEnded.canceled = false;
          app.sendMessage(endMsg);

          Serial.printf("SIMULATOR: Print simulation ended! (%.2fg used). LCD: 'Present tag...'. Scan your spool now to apply deduction.\n", grams);
        } else if (strcmp(cmdBuf, "help") == 0) {
          Serial.println("=== OpenRFID Scanner Commands ===");
          Serial.println("  sim [grams]  : Simulate print cycle (default: 2.10g)");
          Serial.println("  status       : Show device and spool status");
          Serial.println("=================================");
        } else if (strcmp(cmdBuf, "status") == 0) {
          auto& app = ApplicationManager::getInstance();
          Serial.printf("STATUS: AppState=%s, LastSpool=%s, StartingSpool=%s\n",
            app.getState() == AppState::MONITORING_PRINT ? "MONITORING" : "IDLE",
            app.getLastScannedSpoolId(), app.getStartingSpoolId());
        }
      }
      cmdLen = 0;
    } else if (cmdLen + 1 < sizeof(cmdBuf)) {
      cmdBuf[cmdLen++] = static_cast<char>(c);
    }
  }
}

void loop() {
  handleSerialCommands();

  // Process any pending messages for the application
  ApplicationManager::getInstance().processMessages();

  // LCD and NFC scanning are handled by their own tasks
  delay(50);
}

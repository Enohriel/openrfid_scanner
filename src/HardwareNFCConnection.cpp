#include "HardwareNFCConnection.h"
#include <Arduino.h>
#include <cstring>

HardwareNFCConnection::HardwareNFCConnection() {
    memset(currentUid_, 0, sizeof(currentUid_));
}

HardwareNFCConnection::~HardwareNFCConnection() {
    delete nfc_;
}

bool HardwareNFCConnection::begin() {
    // Configure input pins
    pinMode(PN5180_IRQ, INPUT);
    pinMode(PN5180_GPIO, INPUT);
    pinMode(PN5180_AUX, INPUT);

    nfc_ = new PN5180ISO14443(PN5180_NSS, PN5180_BUSY, PN5180_RST,
                               PN5180_SCK, PN5180_MISO, PN5180_MOSI);

    Serial.println("HardwareNFCConnection: Starting PN5180 in ISO14443-A mode...");
    nfc_->begin();
    Serial.println("HardwareNFCConnection: SPI begin done, resetting...");

    // Manual reset with debug
    digitalWrite(PN5180_RST, LOW);
    delay(10);
    digitalWrite(PN5180_RST, HIGH);
    Serial.println("HardwareNFCConnection: RST released, waiting for boot...");

    unsigned long start = millis();
    while (digitalRead(PN5180_BUSY) == HIGH) {
        if (millis() - start > 2000) {
            Serial.println("HardwareNFCConnection: TIMEOUT waiting for BUSY LOW after reset!");
            break;
        }
        delay(1);
    }

    start = millis();
    uint32_t irqStatus = 0;
    while (0 == (irqStatus & (1 << 2))) {  // IDLE_IRQ_STAT
        nfc_->readRegister(IRQ_STATUS, &irqStatus);
        if (millis() - start > 2000) {
            Serial.printf("HardwareNFCConnection: TIMEOUT waiting for IDLE IRQ! IRQ=0x%08lX\n", irqStatus);
            break;
        }
        delay(1);
    }
    nfc_->clearIRQStatus(0xffffffff);
    Serial.println("HardwareNFCConnection: Reset complete");

    // Read firmware version
    uint8_t firmwareVersion[2];
    nfc_->readEEprom(PN5180_FIRMWARE_VERSION, firmwareVersion, 2);
    Serial.printf("HardwareNFCConnection: PN5180 firmware: %d.%d\n",
                  firmwareVersion[1], firmwareVersion[0]);

    // Setup RF for ISO14443-A (NTAG215)
    if (!setupRF()) {
        Serial.println("HardwareNFCConnection: Failed to setup ISO14443-A RF");
        return false;
    }

    Serial.println("HardwareNFCConnection: Initialized ISO14443-A successfully");
    return true;
}

void HardwareNFCConnection::reset() {
    if (nfc_) {
        nfc_->reset();
    }
}

bool HardwareNFCConnection::hardwareReset() {
    if (!nfc_) return false;

    Serial.println("HardwareNFC: hardwareReset() - toggling RST pin");
    digitalWrite(PN5180_RST, LOW);
    delay(10);
    digitalWrite(PN5180_RST, HIGH);

    unsigned long start = millis();
    while (digitalRead(PN5180_BUSY) == HIGH) {
        if (millis() - start > 2000) {
            Serial.println("HardwareNFC: hardwareReset TIMEOUT waiting for BUSY LOW");
            return false;
        }
        delay(1);
    }

    start = millis();
    uint32_t irqStatus = 0;
    while (0 == (irqStatus & (1 << 2))) {  // IDLE_IRQ_STAT
        nfc_->readRegister(IRQ_STATUS, &irqStatus);
        if (millis() - start > 2000) {
            Serial.printf("HardwareNFC: hardwareReset TIMEOUT waiting for IDLE IRQ, IRQ=0x%08lX\n", irqStatus);
            return false;
        }
        delay(1);
    }
    nfc_->clearIRQStatus(0xffffffff);

    return setupRF();
}

bool HardwareNFCConnection::setupRF() {
    if (!nfc_) return false;
    return nfc_->setupRF();
}

bool HardwareNFCConnection::detectTag(uint8_t* uid, uint8_t* uidLength) {
    if (!nfc_) {
        return false;
    }

    int8_t len = nfc_->readCardSerial(uid);
    if (len > 0) {
        *uidLength = len;
        setCurrentUid(uid, len);
        return true;
    }
    return false;
}

void HardwareNFCConnection::setCurrentUid(const uint8_t* uid, uint8_t length) {
    memcpy(currentUid_, uid, length < 7 ? length : 7);
}

bool HardwareNFCConnection::readPage(uint8_t page, uint8_t* buffer4) {
    if (!nfc_) return false;
    for (int retry = 0; retry < 3; retry++) {
        if (nfc_->readNtagPage(page, buffer4)) return true;
        delay(2);
    }
    return false;
}

bool HardwareNFCConnection::readBlock(uint8_t startPage, uint8_t* buffer16) {
    if (!nfc_) return false;
    for (int retry = 0; retry < 3; retry++) {
        if (nfc_->mifareBlockRead(startPage, buffer16)) return true;
        delay(2);
    }
    return false;
}

bool HardwareNFCConnection::writePage(uint8_t page, const uint8_t* data4) {
    if (!nfc_) return false;
    for (int retry = 0; retry < 3; retry++) {
        if (nfc_->writeNtagPage(page, data4)) return true;
        delay(2);
    }
    return false;
}

void HardwareNFCConnection::logDiagnostics() {
    if (!nfc_) return;
    uint32_t rfStatus = 0, irqStatus = 0, sysStatus = 0;
    nfc_->readRegister(RF_STATUS, &rfStatus);
    nfc_->readRegister(IRQ_STATUS, &irqStatus);
    nfc_->readRegister(SYSTEM_STATUS, &sysStatus);
    Serial.printf("HardwareNFC: Diag - RF=0x%08lX, IRQ=0x%08lX, SYS=0x%08lX\n",
                  rfStatus, irqStatus, sysStatus);
}

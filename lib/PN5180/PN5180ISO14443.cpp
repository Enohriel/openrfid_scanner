// NAME: PN5180ISO14443.cpp
//
// DESC: ISO14443 protocol on NXP Semiconductors PN5180 module for Arduino / ESP32.
// Adapted from ATrappmann/PN5180-Library for NTAG215 (Type 2 Tag) operations.
//
#include <Arduino.h>
#include "PN5180ISO14443.h"
#include <PN5180.h>
#include "Debug.h"

PN5180ISO14443::PN5180ISO14443(uint8_t SSpin, uint8_t BUSYpin, uint8_t RSTpin,
                               int8_t SCKpin, int8_t MISOpin, int8_t MOSIpin)
              : PN5180(SSpin, BUSYpin, RSTpin, SCKpin, MISOpin, MOSIpin) {
}

bool PN5180ISO14443::setupRF() {
  Serial.println("PN5180: Loading RF-Configuration ISO14443...");
  if (!loadRFConfig(0x00, 0x80)) {
    Serial.println("PN5180: loadRFConfig failed!");
    return false;
  }

  Serial.println("PN5180: Turning ON RF field...");
  if (!setRF_on()) {
    Serial.println("PN5180: setRF_on warning, continuing...");
  } else {
    Serial.println("PN5180: RF field ON successfully");
  }

  return true;
}

uint16_t PN5180ISO14443::rxBytesReceived() {
  uint32_t rxStatus = 0;
  readRegister(RX_STATUS, &rxStatus);
  return (uint16_t)(rxStatus & 0x000001ff);
}

/*
 * buffer : must be 10 byte array
 * buffer[0-1] is ATQA
 * buffer[2] is sak
 * buffer[3..6] is 4 byte UID
 * buffer[7..9] is remaining 3 bytes of UID for 7 Byte UID tags
 * kind : 0 = REQA, 1 = WUPA
 *
 * return value: UID length (4, 7 or 0 if none)
 */
int8_t PN5180ISO14443::activateTypeA(uint8_t *buffer, uint8_t kind) {
  uint8_t cmd[7];
  uint8_t uidLength = 0;

  // Ensure RF field is ON without cycling it every loop
  uint32_t rfStatus = 0;
  readRegister(RF_STATUS, &rfStatus);
  if (!(rfStatus & (1 << 17))) { // bit 17 = TX_RF_STATUS
    loadRFConfig(0x00, 0x80);
    setRF_on();
    delay(10);
  }

  // OFF Crypto
  if (!writeRegisterWithAndMask(SYSTEM_CONFIG, 0xFFFFFFBF)) return 0;
  // Clear RX CRC
  if (!writeRegisterWithAndMask(CRC_RX_CONFIG, 0xFFFFFFFE)) return 0;
  // Clear TX CRC
  if (!writeRegisterWithAndMask(CRC_TX_CONFIG, 0xFFFFFFFE)) return 0;

  // Set IDLE then TRANSCEIVE command
  if (!writeRegisterWithAndMask(SYSTEM_CONFIG, 0xFFFFFFF8)) return 0;
  if (!writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003)) return 0;

  clearIRQStatus(0xffffffff);

  // Send REQA (0x26) or WUPA (0x52), 7 bits in last byte
  cmd[0] = (kind == 0) ? 0x26 : 0x52;
  if (!sendData(cmd, 1, 0x07)) return 0;

  delay(5);
  if (rxBytesReceived() < 2) return 0;

  // READ 2 bytes ATQA into buffer
  if (!readData(2, buffer)) return 0;

  clearIRQStatus(0xffffffff);

  // Send Anti collision 1, 8 bits in last byte
  cmd[0] = 0x93;
  cmd[1] = 0x20;
  if (!sendData(cmd, 2, 0x00)) return 0;

  delay(5);
  if (rxBytesReceived() != 5) return 0;

  // Read 5 bytes, store at offset 2 for later usage
  if (!readData(5, cmd+2)) return 0;

  // Enable RX CRC calculation
  if (!writeRegisterWithOrMask(CRC_RX_CONFIG, 0x01)) return 0;
  // Enable TX CRC calculation
  if (!writeRegisterWithOrMask(CRC_TX_CONFIG, 0x01)) return 0;

  // Send Select anti collision 1
  cmd[0] = 0x93;
  cmd[1] = 0x70;
  if (!sendData(cmd, 7, 0x00)) return 0;

  delay(5);
  if (rxBytesReceived() < 1) return 0;

  // Read 1 byte SAK into buffer[2]
  if (!readData(1, buffer+2)) return 0;

  // Check if tag is 4 Byte UID or 7 byte UID (Bit 2 == 0 means 4-byte)
  if ((buffer[2] & 0x04) == 0) {
    for (int i = 0; i < 4; i++) buffer[3+i] = cmd[2 + i];
    uidLength = 4;
  } else {
    if (cmd[2] != 0x88) return 0; // Cascade tag
    for (int i = 0; i < 3; i++) buffer[3+i] = cmd[3 + i];

    // Clear RX CRC
    if (!writeRegisterWithAndMask(CRC_RX_CONFIG, 0xFFFFFFFE)) return 0;
    // Clear TX CRC
    if (!writeRegisterWithAndMask(CRC_TX_CONFIG, 0xFFFFFFFE)) return 0;

    // Do anti collision 2
    cmd[0] = 0x95;
    cmd[1] = 0x20;
    if (!sendData(cmd, 2, 0x00)) return 0;

    delay(5);
    if (rxBytesReceived() != 5) return 0;

    // Read 5 bytes
    if (!readData(5, cmd+2)) return 0;

    for (int i = 0; i < 4; i++) {
      buffer[6 + i] = cmd[2+i];
    }

    // Enable RX CRC calculation
    if (!writeRegisterWithOrMask(CRC_RX_CONFIG, 0x01)) return 0;
    // Enable TX CRC calculation
    if (!writeRegisterWithOrMask(CRC_TX_CONFIG, 0x01)) return 0;

    // Send Select anti collision 2
    cmd[0] = 0x95;
    cmd[1] = 0x70;
    if (!sendData(cmd, 7, 0x00)) return 0;

    delay(5);
    if (rxBytesReceived() < 1) return 0;

    // Read 1 byte SAK into buffer[2]
    if (!readData(1, buffer + 2)) return 0;
    uidLength = 7;
  }

  return uidLength;
}

int8_t PN5180ISO14443::readCardSerial(uint8_t *buffer) {
  static uint32_t pollLog = 0;
  if (++pollLog % 40 == 0) {
    Serial.println("PN5180: Polling... (field active, awaiting tag)");
  }

  uint8_t response[10];
  memset(response, 0, sizeof(response));

  // Try WUPA first, then REQA
  uint8_t uidLen = activateTypeA(response, 1);
  if (uidLen == 0) {
    uidLen = activateTypeA(response, 0);
  }

  if (uidLen == 0) return 0;
  if ((response[0] == 0xFF) && (response[1] == 0xFF)) return 0;
  if ((response[3] == 0x00) && (response[4] == 0x00) && (response[5] == 0x00) && (response[6] == 0x00)) return 0;
  if ((response[3] == 0xFF) && (response[4] == 0xFF) && (response[5] == 0xFF) && (response[6] == 0xFF)) return 0;

  for (int i = 0; i < 7; i++) buffer[i] = response[i+3];
  Serial.printf("PN5180: Tag detected! UID (%d bytes): %02X %02X %02X %02X %02X %02X %02X\n",
                uidLen, buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6]);
  return uidLen;
}

bool PN5180ISO14443::isCardPresent() {
  uint8_t buffer[10];
  return (readCardSerial(buffer) >= 4);
}

bool PN5180ISO14443::mifareBlockRead(uint8_t blockno, uint8_t *buffer) {
  uint8_t cmd[2];
  cmd[0] = 0x30;
  cmd[1] = blockno;
  if (!sendData(cmd, 2, 0x00)) return false;

  unsigned long start = millis();
  uint16_t len = 0;
  while (millis() - start < 40) {
    len = rxBytesReceived();
    if (len >= 16) break;
    delay(1);
  }

  if (len >= 16) {
    return readData(16, buffer);
  }
  return false;
}

bool PN5180ISO14443::readNtagPage(uint8_t page, uint8_t *buffer4) {
  uint8_t buf16[16];
  if (mifareBlockRead(page, buf16)) {
    memcpy(buffer4, buf16, 4);
    return true;
  }
  return false;
}

bool PN5180ISO14443::writeNtagPage(uint8_t page, const uint8_t *data4) {
  uint8_t cmd[6];
  cmd[0] = 0xA2;
  cmd[1] = page;
  memcpy(&cmd[2], data4, 4);

  // Disable CRC on RX (NTAG ACK is 4-bit nibble 0x0A without CRC)
  writeRegisterWithAndMask(CRC_RX_CONFIG, 0xFFFFFFFE);
  // Enable CRC on TX (write command has CRC-16)
  writeRegisterWithOrMask(CRC_TX_CONFIG, 0x01);

  clearIRQStatus(0xffffffff);

  if (!sendData(cmd, 6, 0x00)) {
    writeRegisterWithOrMask(CRC_RX_CONFIG, 0x01);
    return false;
  }

  unsigned long start = millis();
  uint16_t len = 0;
  while (millis() - start < 30) {
    len = rxBytesReceived();
    if (len > 0) break;
    delay(1);
  }

  bool success = false;
  if (len > 0) {
    uint8_t ack[2] = {0};
    if (readData(1, ack)) {
      if ((ack[0] & 0x0F) == 0x0A) {
        success = true;
      }
    }
  }

  // Restore CRC on RX for subsequent reads
  writeRegisterWithOrMask(CRC_RX_CONFIG, 0x01);

  if (success) {
    delay(6); // t_WRITE: NTAG EEPROM programming time is max 5ms
  }
  return success;
}

bool PN5180ISO14443::mifareHalt() {
  uint8_t cmd[2] = { 0x50, 0x00 };
  sendData(cmd, 2, 0x00);
  return true;
}

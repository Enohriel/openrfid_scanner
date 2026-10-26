// NAME: PN5180ISO14443.cpp
//
// DESC: ISO14443-A protocol on NXP Semiconductors PN5180 module for Arduino / ESP32.
// Adapted for NTAG215 (Type 2 Tag) operations.
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
    PN5180DEBUG(F("Loading RF-Configuration ISO14443...\n"));
    if (!loadRFConfig(0x00, 0x80)) {
        return false;
    }
    PN5180DEBUG(F("Turning ON RF field...\n"));
    if (!setRF_on()) {
        return false;
    }
    return true;
}

uint16_t PN5180ISO14443::rxBytesReceived() {
    uint32_t rxStatus = 0;
    readRegister(RX_STATUS, &rxStatus);
    return (uint16_t)(rxStatus & 0x000001ff);
}

/*
 * Activates ISO14443-A transponder.
 * buffer must have space for 10 bytes:
 * buffer[0..1] = ATQA, buffer[2] = SAK, buffer[3..9] = UID
 * Returns UID length (4, 7, or 0 if none)
 */
int8_t PN5180ISO14443::activateTypeA(uint8_t *buffer, uint8_t kind) {
    uint8_t cmd[7];
    uint8_t uidLength = 0;

    if (!loadRFConfig(0x00, 0x80)) {
        return -1;
    }

    // Disable crypto
    if (!writeRegisterWithAndMask(SYSTEM_CONFIG, 0xFFFFFFBF)) {
        return -1;
    }
    // Disable RX CRC for short frame REQA/WUPA
    if (!writeRegisterWithAndMask(CRC_RX_CONFIG, 0xFFFFFFFE)) {
        return -1;
    }
    // Disable TX CRC
    if (!writeRegisterWithAndMask(CRC_TX_CONFIG, 0xFFFFFFFE)) {
        return -1;
    }

    // REQA = 0x26, WUPA = 0x52 (7 bits)
    cmd[0] = (kind == 0) ? 0x26 : 0x52;
    if (!sendData(cmd, 1, 0x07)) {
        return 0;
    }

    uint16_t len = rxBytesReceived();
    if (len != 2) {
        return 0;
    }

    // Read ATQA (2 bytes)
    readData(2, buffer);

    // Enable RX and TX CRC for subsequent commands
    writeRegisterWithOrMask(CRC_RX_CONFIG, 0x00000001);
    writeRegisterWithOrMask(CRC_TX_CONFIG, 0x00000001);

    // --- Cascade Level 1 ---
    cmd[0] = 0x93; // SELECT Cascade Level 1
    cmd[1] = 0x20; // NVB = 2 bytes
    if (!sendData(cmd, 2, 0x00)) {
        return 0;
    }

    len = rxBytesReceived();
    if (len < 5) {
        return 0;
    }

    uint8_t cl1_resp[5];
    readData(5, cl1_resp);

    // Complete Cascade Level 1 selection
    cmd[0] = 0x93;
    cmd[1] = 0x70;
    memcpy(&cmd[2], cl1_resp, 5);
    if (!sendData(cmd, 7, 0x00)) {
        return 0;
    }

    len = rxBytesReceived();
    if (len < 1) {
        return 0;
    }

    uint8_t sak = 0;
    readData(1, &sak);
    buffer[2] = sak;

    // Check if cascade bit is set in SAK (bit 3 / 0x04)
    if (sak & 0x04) {
        // UID not complete -> Cascade Level 2 (e.g. NTAG215 7-byte UID)
        // Store first 3 UID bytes (skip 0x88 cascade tag)
        buffer[3] = cl1_resp[1];
        buffer[4] = cl1_resp[2];
        buffer[5] = cl1_resp[3];

        // --- Cascade Level 2 ---
        cmd[0] = 0x95; // SELECT Cascade Level 2
        cmd[1] = 0x20;
        if (!sendData(cmd, 2, 0x00)) {
            return 0;
        }

        len = rxBytesReceived();
        if (len < 5) {
            return 0;
        }

        uint8_t cl2_resp[5];
        readData(5, cl2_resp);

        // Complete Cascade Level 2 selection
        cmd[0] = 0x95;
        cmd[1] = 0x70;
        memcpy(&cmd[2], cl2_resp, 5);
        if (!sendData(cmd, 7, 0x00)) {
            return 0;
        }

        len = rxBytesReceived();
        if (len < 1) {
            return 0;
        }
        readData(1, &sak);
        buffer[2] = sak;

        // Next 4 UID bytes
        buffer[6] = cl2_resp[0];
        buffer[7] = cl2_resp[1];
        buffer[8] = cl2_resp[2];
        buffer[9] = cl2_resp[3];

        uidLength = 7;
    } else {
        // Single size UID (4 bytes)
        buffer[3] = cl1_resp[0];
        buffer[4] = cl1_resp[1];
        buffer[5] = cl1_resp[2];
        buffer[6] = cl1_resp[3];
        uidLength = 4;
    }

    return uidLength;
}

int8_t PN5180ISO14443::readCardSerial(uint8_t *buffer) {
    uint8_t raw[10];
    int8_t uidLen = activateTypeA(raw, 0);
    if (uidLen <= 0) {
        return 0;
    }

    // Check for valid non-dummy UID
    bool valid = false;
    for (int i = 0; i < uidLen; i++) {
        if (raw[3 + i] != 0x00 && raw[3 + i] != 0xFF) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        return 0;
    }

    memcpy(buffer, &raw[3], uidLen);
    return uidLen;
}

bool PN5180ISO14443::isCardPresent() {
    uint8_t uid[10];
    return (readCardSerial(uid) >= 4);
}

bool PN5180ISO14443::mifareBlockRead(uint8_t blockno, uint8_t *buffer16) {
    uint8_t cmd[2];
    cmd[0] = 0x30; // READ 16 bytes (4 pages on NTAG)
    cmd[1] = blockno;
    if (!sendData(cmd, 2, 0x00)) {
        return false;
    }
    delay(5);
    uint16_t len = rxBytesReceived();
    if (len >= 16) {
        readData(16, buffer16);
        return true;
    }
    return false;
}

bool PN5180ISO14443::readNtagPage(uint8_t page, uint8_t *buffer4) {
    uint8_t buf16[16];
    if (!mifareBlockRead(page, buf16)) {
        return false;
    }
    memcpy(buffer4, buf16, 4);
    return true;
}

bool PN5180ISO14443::writeNtagPage(uint8_t page, const uint8_t *data4) {
    uint8_t cmd[6];
    cmd[0] = 0xA2; // NTAG WRITE (4 bytes)
    cmd[1] = page;
    cmd[2] = data4[0];
    cmd[3] = data4[1];
    cmd[4] = data4[2];
    cmd[5] = data4[3];

    // Clear RX CRC because ACK is only 4 bits (half byte)
    writeRegisterWithAndMask(CRC_RX_CONFIG, 0xFFFFFFFE);

    if (!sendData(cmd, 6, 0x00)) {
        // Re-enable CRC before returning
        writeRegisterWithOrMask(CRC_RX_CONFIG, 0x00000001);
        return false;
    }

    delay(6); // Wait for NTAG EEPROM programming (max 4.1ms)
    uint8_t ack[2] = {0};
    uint8_t *res = readData(1, ack);

    // Re-enable RX CRC
    writeRegisterWithOrMask(CRC_RX_CONFIG, 0x00000001);

    if (res && ((ack[0] & 0x0F) == 0x0A)) {
        return true;
    }
    return false;
}

bool PN5180ISO14443::mifareHalt() {
    uint8_t cmd[2] = { 0x50, 0x00 };
    return sendData(cmd, 2, 0x00);
}

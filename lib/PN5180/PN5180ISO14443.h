// NAME: PN5180ISO14443.h
//
// DESC: ISO14443-A protocol on NXP Semiconductors PN5180 module for Arduino / ESP32.
// Adapted for NTAG215 (Type 2 Tag) read/write operations.
//
#ifndef PN5180ISO14443_H
#define PN5180ISO14443_H

#include "PN5180.h"

class PN5180ISO14443 : public PN5180 {
public:
  PN5180ISO14443(uint8_t SSpin, uint8_t BUSYpin, uint8_t RSTpin,
                 int8_t SCKpin = -1, int8_t MISOpin = -1, int8_t MOSIpin = -1);

  bool setupRF();

  // Activates Type A tag. Returns UID length (4, 7 or 0 if no tag).
  // buffer must be at least 10 bytes:
  // buffer[0..1] = ATQA, buffer[2] = SAK, buffer[3..9] = UID
  int8_t activateTypeA(uint8_t *buffer, uint8_t kind = 0);

  // Helper: fills buffer with UID bytes and returns UID length (e.g. 7 for NTAG215)
  int8_t readCardSerial(uint8_t *buffer);
  bool isCardPresent();

  // Mifare / NTAG 16-byte block read (cmd 0x30, reads 4 pages at blockno)
  bool mifareBlockRead(uint8_t blockno, uint8_t *buffer16);

  // NTAG215 single page (4 bytes) read
  bool readNtagPage(uint8_t page, uint8_t *buffer4);

  // NTAG215 single page (4 bytes) write (cmd 0xA2)
  bool writeNtagPage(uint8_t page, const uint8_t *data4);

  bool mifareHalt();
  bool transceive(uint8_t *cmd, int cmdLen, uint8_t validBits = 0, uint32_t timeoutMs = 15);

private:
  uint16_t rxBytesReceived();
};

#endif /* PN5180ISO14443_H */

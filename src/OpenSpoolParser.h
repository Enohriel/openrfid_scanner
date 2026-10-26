#ifndef OPEN_SPOOL_PARSER_H
#define OPEN_SPOOL_PARSER_H

#include <cstdint>
#include <cstddef>
#include "NFCConnectionI.h"
#include "NFCTypes.h"

class OpenSpoolParser {
public:
    // Reads NTAG215 user pages, extracts and parses OpenSpool NDEF JSON
    static bool readFromTag(NFCConnectionI* conn, OpenSpoolData* outData);

    // Formats and writes OpenSpoolData as a standard NDEF MIME JSON record to NTAG215
    static bool writeToTag(NFCConnectionI* conn, const OpenSpoolData* inData);

    // Parses raw buffer containing NDEF / JSON into OpenSpoolData
    static bool parseJsonPayload(const char* jsonStr, size_t len, OpenSpoolData* outData);

    // Serializes OpenSpoolData into a JSON string
    static size_t serializeJsonPayload(const OpenSpoolData* inData, char* outBuffer, size_t maxLen);
};

#endif // OPEN_SPOOL_PARSER_H

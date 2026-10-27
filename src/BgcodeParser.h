#ifndef BGCODE_PARSER_H
#define BGCODE_PARSER_H

#include <cstdint>
#include <cstddef>

struct GcodeStreamState {
    char lineBuf[384] = {0};
    size_t lineLen = 0;
    bool inThumbnail = false;
    bool done = false;
    int linesRead = 0;
    int bestPriority = 0;   // 0=none, 1=mm/cm3 fallback, 2=filament used [g], 3=total filament used [g]
    float bestFilamentG = 0.0f;
};

// Parse a single G-code comment line for filament weight.
// outPriority: 3 = total [g], 2 = single [g], 1 = [cm3]/[mm] fallback, 0 = no match.
float parseGcodeCommentLine(const char* line, size_t len, int& outPriority);

// Check if a G-code comment line starts the trailing slicer configuration dump.
bool isGcodeConfigBlockStart(const char* line);

// Feed a chunk of text G-code bytes into the streaming parser.
float feedGcodeStreamChunk(GcodeStreamState& st, const uint8_t* chunk, size_t chunkLen);

// Flush any partial final line at EOF and return the best parsed filament weight in grams.
float finalizeGcodeStream(GcodeStreamState& st);

// Parse a bgcode file header or text G-code buffer to extract filament weight in grams.
// Returns the filament weight in grams, or 0.0f if not found.
float parseBgcodeFilament(const uint8_t* data, size_t len);

#endif // BGCODE_PARSER_H

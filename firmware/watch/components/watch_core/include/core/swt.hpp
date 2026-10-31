// SWT map tiles (see server/src/tiles.rs): header + zlib(4-bit indexed pixels). Parsing and
// palette expansion live here (host-testable); inflating uses the ESP32-S3 ROM tinfl.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

constexpr int kSwtSize = 256;
constexpr size_t kSwtIndexBytes = kSwtSize * kSwtSize / 2;  // 4 bits per pixel

struct SwtHeader {
    uint16_t palette565[16];
    size_t payload_offset = 0;  // start of the zlib stream
};

// Validates a 256x256, 4-bit, <=16 colour SWT tile and converts its palette to RGB565.
bool parse_swt(const uint8_t* data, size_t len, SwtHeader& out);

// Expands packed 4-bit indices (left pixel in the high nibble) into RGB565 pixels.
void expand_i4(const uint8_t* indices, const uint16_t* palette565, uint16_t* out, size_t pixels);

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

}  // namespace core

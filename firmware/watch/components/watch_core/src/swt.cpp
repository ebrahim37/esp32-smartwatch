#include "core/swt.hpp"

#include <cstring>

namespace core {

bool parse_swt(const uint8_t* d, size_t len, SwtHeader& out) {
    if (len < 10 || std::memcmp(d, "SWT1", 4) != 0) return false;
    uint16_t w = d[4] | (d[5] << 8), h = d[6] | (d[7] << 8);
    uint8_t fmt = d[8], colours = d[9];
    if (w != kSwtSize || h != kSwtSize || fmt != 1 || colours == 0 || colours > 16) return false;
    size_t pal_end = 10 + static_cast<size_t>(colours) * 3;
    if (len <= pal_end) return false;
    for (int i = 0; i < 16; ++i) {
        const uint8_t* c = d + 10 + (i < colours ? i : 0) * 3;
        out.palette565[i] = rgb565(c[0], c[1], c[2]);
    }
    out.payload_offset = pal_end;
    return true;
}

void expand_i4(const uint8_t* idx, const uint16_t* pal, uint16_t* out, size_t pixels) {
    for (size_t i = 0; i + 1 < pixels; i += 2) {
        uint8_t b = idx[i / 2];
        out[i] = pal[b >> 4];
        out[i + 1] = pal[b & 0x0F];
    }
}

}  // namespace core

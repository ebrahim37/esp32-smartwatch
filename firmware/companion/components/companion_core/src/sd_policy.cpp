#include "ccore/sd_policy.hpp"

#include <cstring>

namespace ccore {

namespace {
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
}  // namespace

const char* volume_kind_name(VolumeKind k) {
    switch (k) {
        case VolumeKind::Blank: return "blank";
        case VolumeKind::Fat32: return "FAT32";
        case VolumeKind::FatOther: return "FAT12/16";
        case VolumeKind::ExFat: return "exFAT";
        case VolumeKind::Ntfs: return "NTFS";
        case VolumeKind::LinuxOrOther: return "non-FAT partition";
        default: return "unknown";
    }
}

bool sector_blank(const uint8_t* s) {
    bool zeros = true, ones = true;
    for (int i = 0; i < 512; ++i) {
        zeros &= s[i] == 0x00;
        ones &= s[i] == 0xFF;
    }
    return zeros || ones;
}

VolumeKind classify_vbr(const uint8_t* s) {
    if (sector_blank(s)) return VolumeKind::Blank;
    if (std::memcmp(s + 3, "EXFAT   ", 8) == 0) return VolumeKind::ExFat;
    if (std::memcmp(s + 3, "NTFS    ", 8) == 0) return VolumeKind::Ntfs;
    if (s[510] != 0x55 || s[511] != 0xAA) return VolumeKind::Unknown;
    uint16_t bps = le16(s + 11);
    uint8_t spc = s[13];
    uint16_t reserved = le16(s + 14);
    uint8_t nfats = s[16];
    bool bpb_ok = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && spc != 0 && (spc & (spc - 1)) == 0 &&
                  reserved != 0 && (nfats == 1 || nfats == 2);
    if (!bpb_ok) return VolumeKind::Unknown;
    uint16_t fat16_size = le16(s + 22);
    uint32_t fat32_size = le32(s + 36);
    if (fat16_size == 0 && fat32_size != 0 && std::memcmp(s + 82, "FAT32   ", 8) == 0) return VolumeKind::Fat32;
    if (fat16_size == 0 && fat32_size != 0) return VolumeKind::Fat32;  // label is informational only
    if (fat16_size != 0) return VolumeKind::FatOther;
    return VolumeKind::Unknown;
}

VolumeKind classify_sector0(const uint8_t* s, uint32_t& partition_lba, uint8_t& partition_type) {
    partition_lba = 0;
    partition_type = 0;
    if (sector_blank(s)) return VolumeKind::Blank;
    // A VBR at sector 0 (superfloppy layout) starts with a jump instruction.
    VolumeKind vbr = classify_vbr(s);
    if ((s[0] == 0xEB || s[0] == 0xE9) && vbr != VolumeKind::Unknown) return vbr;
    if (s[510] != 0x55 || s[511] != 0xAA) return VolumeKind::Unknown;
    // MBR: take the first non-empty partition entry.
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = s + 446 + 16 * i;
        uint8_t type = e[4];
        uint32_t lba = le32(e + 8);
        uint32_t count = le32(e + 12);
        if (type == 0 || count == 0) continue;
        partition_type = type;
        partition_lba = lba;
        switch (type) {
            case 0x0B: case 0x0C: return VolumeKind::Fat32;   // confirmed later via the VBR
            case 0x01: case 0x04: case 0x06: case 0x0E: return VolumeKind::FatOther;
            case 0x07: return VolumeKind::Unknown;            // NTFS or exFAT: decided by the VBR
            case 0xEE: return VolumeKind::LinuxOrOther;       // GPT protective MBR
            default: return VolumeKind::LinuxOrOther;
        }
    }
    // Boot signature but no partitions: treat as unrecognised, never as blank.
    return VolumeKind::Unknown;
}

SdDecision decide(const SdProbe& p) {
    if (!p.card_present) return {SdAction::Refuse, false, "no card detected"};
    if (!p.read_ok) return {SdAction::Refuse, false, "card read failed; not formatting"};
    switch (p.kind) {
        case VolumeKind::Fat32: return {SdAction::Mount, false, "existing FAT32 volume"};
        case VolumeKind::FatOther: return {SdAction::Mount, false, "existing FAT12/16 volume (mounted as is)"};
        case VolumeKind::Blank: return {SdAction::Format, false, "blank card"};
        case VolumeKind::ExFat:
        case VolumeKind::Ntfs:
        case VolumeKind::LinuxOrOther:
            return {SdAction::Format, true, std::string("unsupported filesystem: ") + volume_kind_name(p.kind)};
        default: return {SdAction::Refuse, false, "unrecognised card contents; not formatting"};
    }
}

}  // namespace ccore

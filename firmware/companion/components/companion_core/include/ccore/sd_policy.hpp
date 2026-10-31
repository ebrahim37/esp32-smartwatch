// Decides, from raw sectors read off the card, whether the companion may mount the card as
// is, must format it as FAT32, or must leave it alone.
//
// Safety rule: formatting is only ever chosen when the card was *read successfully* and is
// either conclusively blank or conclusively carries a filesystem we do not support. Card
// absence, I/O errors, unrecognised/ambiguous contents and failed mounts of a FAT volume
// never lead to formatting.
#pragma once

#include <cstdint>
#include <string>

namespace ccore {

enum class VolumeKind {
    Blank,         // no partition table, no boot record, zeroed/erased sectors
    Fat32,
    FatOther,      // FAT12/16: FatFs mounts it, left untouched
    ExFat,
    Ntfs,
    LinuxOrOther,  // MBR/GPT partition with a non-FAT type id (ext4, swap, ...)
    Unknown,       // readable but unrecognised: do not touch
};

const char* volume_kind_name(VolumeKind k);

// Classifies a 512-byte sector that is either an MBR or a volume boot record.
// `partition_lba` receives the first partition's start (0 if the sector is a VBR itself).
VolumeKind classify_sector0(const uint8_t* sector, uint32_t& partition_lba, uint8_t& partition_type);
// Classifies a volume boot record (e.g. the sector at partition_lba).
VolumeKind classify_vbr(const uint8_t* sector);
bool sector_blank(const uint8_t* sector);

enum class SdAction { Mount, Format, Refuse };

struct SdProbe {
    bool card_present = false;
    bool read_ok = false;          // all probe reads succeeded
    VolumeKind kind = VolumeKind::Unknown;
};

struct SdDecision {
    SdAction action = SdAction::Refuse;
    bool destroys_data = false;  // formatting a card that had a filesystem
    std::string reason;
};

SdDecision decide(const SdProbe& p);

// After the decision was Mount but mounting failed: never escalate to formatting.
inline SdDecision on_mount_failure() { return {SdAction::Refuse, false, "FAT volume failed to mount; leaving card untouched"}; }

}  // namespace ccore

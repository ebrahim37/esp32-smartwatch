// Persistent watch configuration in the dedicated `nvs_user` partition.
// Writes are rare and explicit (settings changes, pairing, alarm edits, a 10-minute step
// checkpoint) to limit flash wear.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/alarm_schedule.hpp"
#include "swp/messages.hpp"

namespace settings {

struct Peer {
    bool valid = false;
    uint8_t addr[6] = {};
    uint8_t addr_type = 0;
    std::string name;
};

void init();

swp::WatchSettings get();
void set(const swp::WatchSettings& s);  // persists and applies timezone

Peer peer();
void set_peer(const Peer& p);
void clear_peer();

std::vector<core::Alarm> alarms();
void set_alarms(const std::vector<core::Alarm>& a);

struct StepCheckpoint {
    uint32_t steps = 0;
    int32_t day_key = 0;  // local yyyymmdd the count belongs to
};
StepCheckpoint steps();
void set_steps(const StepCheckpoint& c);

std::vector<uint8_t> blob(const char* key);
void set_blob(const char* key, const std::vector<uint8_t>& data);

}  // namespace settings

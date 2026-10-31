#include "position.hpp"

#include <ctime>

#include "sdkconfig.h"

namespace position {
namespace {

// TODO(PROTOTYPE): dummy fixed coordinates from Kconfig (see Kconfig.projbuild).
class SimulatedProvider : public Provider {
public:
    bool current(swp::Position& out) override {
        out.lat_e7 = CONFIG_COMPANION_SIM_LAT_E7;
        out.lon_e7 = CONFIG_COMPANION_SIM_LON_E7;
        out.accuracy_m = 10;
        out.source = static_cast<uint8_t>(swp::PositionSource::Simulated);
        out.fix_time = static_cast<uint32_t>(time(nullptr));
        return true;
    }
    const char* name() const override { return "simulated"; }
};

SimulatedProvider sim;
Provider* active = &sim;

}  // namespace

void init() { active = &sim; }
bool current(swp::Position& out) { return active->current(out); }
const char* provider_name() { return active->name(); }

}  // namespace position

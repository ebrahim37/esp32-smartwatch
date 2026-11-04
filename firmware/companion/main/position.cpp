#include "position.hpp"

#include <ctime>

#include "sdkconfig.h"

namespace position {

// TODO(PROTOTYPE): dummy fixed coordinates from Kconfig (see Kconfig.projbuild).
bool current(swp::Position& out) {
    out.lat_e7 = CONFIG_COMPANION_SIM_LAT_E7;
    out.lon_e7 = CONFIG_COMPANION_SIM_LON_E7;
    out.accuracy_m = 10;
    out.source = static_cast<uint8_t>(swp::PositionSource::Simulated);
    out.fix_time = static_cast<uint32_t>(time(nullptr));
    return true;
}

}  // namespace position

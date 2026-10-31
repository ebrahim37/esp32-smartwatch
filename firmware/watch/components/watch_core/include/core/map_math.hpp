// Web Mercator helpers for 256 px slippy-map tiles.
#pragma once

#include <cstdint>

namespace core {

constexpr int kTileSize = 256;
constexpr int kMinZoom = 3;
constexpr int kMaxZoom = 18;

struct WorldPx {
    double x, y;  // global pixel coordinates at a given zoom
};

WorldPx latlon_to_px(double lat, double lon, int zoom);
void px_to_latlon(const WorldPx& p, int zoom, double& lat, double& lon);
double meters_per_px(double lat, int zoom);
double haversine_m(double lat1, double lon1, double lat2, double lon2);

inline double e7_to_deg(int32_t v) { return v / 1e7; }
inline int32_t deg_to_e7(double d) { return static_cast<int32_t>(d * 1e7 + (d >= 0 ? 0.5 : -0.5)); }

}  // namespace core

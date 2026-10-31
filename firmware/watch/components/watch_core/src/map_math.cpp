#include "core/map_math.hpp"

#include <cmath>

namespace core {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxLat = 85.05112878;
}  // namespace

WorldPx latlon_to_px(double lat, double lon, int zoom) {
    if (lat > kMaxLat) lat = kMaxLat;
    if (lat < -kMaxLat) lat = -kMaxLat;
    double n = static_cast<double>(kTileSize) * std::ldexp(1.0, zoom);
    double x = (lon + 180.0) / 360.0 * n;
    double s = std::sin(lat * kPi / 180.0);
    double y = (0.5 - std::log((1 + s) / (1 - s)) / (4 * kPi)) * n;
    return {x, y};
}

void px_to_latlon(const WorldPx& p, int zoom, double& lat, double& lon) {
    double n = static_cast<double>(kTileSize) * std::ldexp(1.0, zoom);
    lon = p.x / n * 360.0 - 180.0;
    double m = kPi - 2.0 * kPi * p.y / n;
    lat = 180.0 / kPi * std::atan(std::sinh(m));
}

double meters_per_px(double lat, int zoom) {
    return 156543.03392 * std::cos(lat * kPi / 180.0) / std::ldexp(1.0, zoom);
}

double haversine_m(double lat1, double lon1, double lat2, double lon2) {
    double r = 6371000.0;
    double p1 = lat1 * kPi / 180, p2 = lat2 * kPi / 180;
    double dp = p2 - p1, dl = (lon2 - lon1) * kPi / 180;
    double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * r * std::asin(std::sqrt(a));
}

}  // namespace core

#include "ccore/subsonic.hpp"

#include <cctype>

#include "ccore/md5.hpp"

namespace ccore {

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

std::string subsonic_url(const std::string& base, const std::string& method, const SubsonicAuth& a,
                         const std::string& salt, const std::string& extra) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    std::string url = b + "/rest/" + method + "?u=" + url_encode(a.user) + "&t=" + md5_hex(a.password + salt) +
                      "&s=" + url_encode(salt) + "&v=" + url_encode(a.api_version) + "&c=" + url_encode(a.client) + "&f=json";
    if (!extra.empty()) url += "&" + extra;
    return url;
}

}  // namespace ccore

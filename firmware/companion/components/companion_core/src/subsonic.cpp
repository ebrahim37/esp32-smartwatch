#include "ccore/subsonic.hpp"

#include <cctype>
#include <string_view>

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

bool lan_http_url(const std::string& url) {
    std::string_view host = url;
    if (host.substr(0, 7) != "http://") return false;
    host.remove_prefix(7);
    host = host.substr(0, host.find_first_of(":/?#"));
    unsigned o[4];
    for (int i = 0; i < 4; ++i) {
        size_t dot = host.find('.');
        std::string_view part = host.substr(0, dot);
        if (part.empty() || part.size() > 3) return false;
        o[i] = 0;
        for (char c : part) {
            if (c < '0' || c > '9') return false;
            o[i] = o[i] * 10 + (c - '0');
        }
        if (o[i] > 255 || (dot == std::string_view::npos) != (i == 3)) return false;
        if (i < 3) host.remove_prefix(dot + 1);
    }
    return o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168);
}

}  // namespace ccore

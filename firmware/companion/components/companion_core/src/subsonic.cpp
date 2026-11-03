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

bool lan_http_url(const std::string& url) {
    const std::string p = "http://";
    if (url.compare(0, p.size(), p) != 0) return false;
    size_t end = url.find_first_of(":/?#", p.size());
    std::string host = url.substr(p.size(), end == std::string::npos ? std::string::npos : end - p.size());
    unsigned o[4];
    int n = 0;
    size_t pos = 0;
    for (; n < 4; ++n) {
        size_t dot = host.find('.', pos);
        std::string part = host.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos) return false;
        o[n] = static_cast<unsigned>(std::stoul(part));
        if (o[n] > 255) return false;
        if (dot == std::string::npos) {
            ++n;
            break;
        }
        pos = dot + 1;
    }
    if (n != 4 || host.find('.', pos) != std::string::npos) return false;
    return o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168);
}

}  // namespace ccore

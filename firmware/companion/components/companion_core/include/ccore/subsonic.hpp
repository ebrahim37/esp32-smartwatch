// Subsonic/Navidrome request construction (token authentication, no plaintext password on
// the wire). See http://www.subsonic.org/pages/api.jsp.
#pragma once

#include <string>

namespace ccore {

std::string url_encode(const std::string& s);

struct SubsonicAuth {
    std::string user;
    std::string password;  // never sent; only md5(password + salt) is
    std::string client = "esp32-companion";
    std::string api_version = "1.16.1";
};

// Builds "<base>/rest/<method>?u=..&t=..&s=..&v=..&c=..&f=json[&extra]".
// `salt` must be fresh random hex per request.
std::string subsonic_url(const std::string& base, const std::string& method, const SubsonicAuth& auth,
                         const std::string& salt, const std::string& extra_query = "");

}  // namespace ccore

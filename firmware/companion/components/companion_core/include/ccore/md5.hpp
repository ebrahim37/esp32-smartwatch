// Small MD5 implementation for Subsonic token auth (token = md5(password + salt)).
// MD5 is mandated by the Subsonic API; it is not used for any other security purpose.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ccore {

void md5(const uint8_t* data, size_t len, uint8_t out[16]);
std::string md5_hex(const std::string& s);

}  // namespace ccore

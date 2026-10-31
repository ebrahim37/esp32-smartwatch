// HTTPS client helpers. TLS certificates are always verified against the bundled Mozilla CA
// set (esp_crt_bundle); plain http:// URLs are refused.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace http {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Result {
    int status = 0;      // HTTP status, or 0 on transport error
    std::string error;   // transport error description
    bool ok() const { return status >= 200 && status < 300; }
};

// GET into memory, rejecting bodies larger than max_len.
Result get(const std::string& url, const Headers& headers, std::vector<uint8_t>& body, size_t max_len);
Result post_json(const std::string& url, const Headers& headers, const std::string& json, std::vector<uint8_t>& body, size_t max_len);

// Streams a GET into `path`. If `resume_from` > 0 a Range request continues an earlier
// partial download (the server must answer 206; a 200 restarts from zero).
// `should_abort` is polled between chunks (e.g. playback started, Wi-Fi lost).
Result download(const std::string& url, const Headers& headers, const std::string& path, uint64_t resume_from,
                uint64_t expected_size, const std::function<bool()>& should_abort, uint64_t* bytes_written);

}  // namespace http

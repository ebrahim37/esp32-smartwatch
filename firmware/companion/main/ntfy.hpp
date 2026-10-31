// Direct ntfy subscription: one long-lived HTTPS JSON stream for all configured topics
// (GET <ntfy_url>/<t1,t2>/json?since=<cursor>), with the reverse proxy's auth header.
// ntfy pushes messages and a keepalive every ~45 s over the same TLS connection, so there is
// no per-poll handshake. Messages go to cloud::ingest_notification(); the resume cursor only
// advances once the watch has acknowledged them (persisted in NVS).
#pragma once

#include <cstdint>
#include <string>

namespace ntfy {

void start();
// Watch acknowledged notification `key` (swp::Notification::id).
void on_ack(uint64_t key);
uint32_t last_event_utc();  // last message/keepalive from ntfy, 0 if never
bool connected();

}  // namespace ntfy

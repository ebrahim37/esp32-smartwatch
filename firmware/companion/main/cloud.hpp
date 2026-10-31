// VPS helper client (weather, maps: tiles, POI search, routing) and the notification queue
// towards the watch (fed by ntfy.cpp).
// All network work runs on the cloud worker task; BLE callbacks only enqueue jobs.
#pragma once

#include <cstdint>

#include "swp/messages.hpp"

namespace cloud {

void start();

// Jobs from the BLE dispatcher (non-blocking).
void request_tile(uint16_t conn, const swp::MapTileRequest& r);
void on_transfer_ack(uint16_t conn, const swp::TransferAck& a);
void request_search(uint16_t conn, const swp::MapSearchRequest& r);
void request_route(uint16_t conn, const swp::RouteRequest& r);
void request_weather(uint16_t conn);
void request_map_sync(uint16_t conn, const swp::MapSyncRequest& r);   // offline tiles -> SD
void request_map_data(uint16_t conn, const swp::MapDataCommand& c);   // helper OSM data

void ingest_notification(const swp::Notification& n);  // from the ntfy stream
void on_notification_ack(uint64_t id);
void on_watch_connected();
void on_watch_disconnected();

bool weather(swp::Weather& out);
uint16_t pending_notifications();
bool server_ok();

}  // namespace cloud

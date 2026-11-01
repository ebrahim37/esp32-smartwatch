# VPS helper (`server/`)

Rust (Tokio + Axum). Endpoints are documented in `server/src/app.rs`; all `/v1/*` routes
require `Authorization: Bearer $DEVICE_TOKEN` (constant-time compare). Upstream credentials
stay in the server environment.

## Configure

All settings are environment variables, listed with defaults in
[server/README.md](../server/README.md) together with container build/run examples and a
reference deployment. Generate the
device token with `openssl rand -base64 32` and enter the same value on the watch
(Settings → Server → Device token).

Data sources (pick services whose terms allow your use; self-hosting is recommended):

| Feature | Default | Notes |
|---------|---------|-------|
| Weather | Open-Meteo forecast API | cached 15 min per ~1 km cell |
| Tiles | `TILE_URL_TEMPLATE` (required) | must be self-hosted or a provider permitting caching; `tile.openstreetmap.org` is refused; rendered at 2x from the zoom-1 tile (`TILE_SCALE`) |
| Search | local index of the map area (below), then Photon | Photon fills in outside the area or when there are fewer than 3 local matches |
| Nearby POIs | local index (named places within 1 km), then Overpass | light use only on the public Overpass instance |
| Routing | OSRM (`OSRM_URL`, `{profile}` placeholder supported) | self-host per profile (foot/bicycle/driving) |
| Long routes | FOSSGIS OSRM (`routing.openstreetmap.de`) | destinations outside the local area; ≤1 request/s, light use; `ROUTE_FALLBACK=off` disables |

Attribution: "© OpenStreetMap contributors" is returned with tiles/search and shown on the watch.

## Behind Caddy

Caddyfile:

```
watch.example.com {
    reverse_proxy 127.0.0.1:47913
}
```

TLS terminates at Caddy (public certificate), so the companion's certificate-bundle
verification succeeds. Test: `curl -H "Authorization: Bearer $TOKEN" https://watch.example.com/v1/status`.


## Notifications: companion → ntfy directly

The helper is not involved in notifications; the companion subscribes to ntfy itself:
`GET <ntfy_url>/<topic1,topic2>/json?since=<cursor>` with an `Auth: <ntfy_token>` header that
the reverse proxy checks (read-only, GET only). ntfy pushes each message and a keepalive about
every 45 s over that one TLS connection, so idle cost is a few small TLS records per minute
rather than a full HTTPS request (and often a TLS handshake) per poll.

Delivery is at-least-once: the resume cursor (persisted in companion NVS) only advances past
messages the watch has acknowledged, and replays are de-duplicated by message id. The first
subscription uses `since=10m`. Topics are edited on the watch (Settings → Notifications) or
via the `ntfy_topics` config key; `ntfy_url` and `ntfy_token` are set the same way.

## Maps: on-demand OSM data and SWT tiles

With `MAPCACHE_DIR` set, the helper builds map data where the watch actually is. The first
tile (zoom ≥ 10) or route outside the current area answers `503` with `Retry-After: 60`
while the helper:

1. picks the smallest Geofabrik extract containing a `MAP_AREA_KM` square around the point
   (from Geofabrik's `index-v1.json`, refreshed weekly),
2. downloads it into a ring cache capped at `MAPCACHE_MAX_GB` (oldest extracts/areas are
   deleted first; the active area never is),
3. clips the square with osmium, builds OSRM graphs (foot, bicycle, car), a search index of
   named places (`pois.json`: amenities, shops, tourism, transit, places; built on first
   search for areas made before it existed) and OpenMapTiles vector tiles with Planetiler,
4. switches `MAPCACHE_DIR/active` to the new area. The tile server (tileserver-gl) and
   routing (osrm-routed) containers watch that link and reload.

A 10 km test area built in about 80 s; Planetiler also keeps ~1.4 GB of water/Natural Earth
sources in the cache. `POST /v1/maps/data/update {lat,lon}` re-downloads and rebuilds an area,
`POST /v1/maps/data/delete` clears everything (both available on the watch under
Settings → Maps).

Tiles go over the wire as **SWT**: a 58-byte header (size, 16-colour palette) and a zlib
stream of 4-bit pixels, ~6 KB per 256 px tile. The companion stores and forwards the bytes;
the watch inflates them with the ESP32-S3 ROM `tinfl` straight into an RGB565 buffer that
LVGL draws directly (no PNG decoder). `GET /v1/maps/bundle?lat&lon&radius_m&zmin&zmax`
streams many tiles in one response (`"SWB1"`, then `z u8, x u32, y u32, len u32, SWT` per
tile, `0xFF` at the end, rendered four at a time); the companion uses it to keep the area
around its position (5 km, zoom 12–16) in its SD cache, refreshed after moving 2 km or
weekly, so an offline area costs one HTTPS request instead of one per tile.

Interactive tiles take one request each over a kept-alive HTTPS connection (one TLS
handshake per session, not per tile). The companion fetches up to two tiles ahead of the
BLE transfer, sends before writing to SD, and the watch shortens the BLE connection interval
(15–20 ms, 251-byte packets) while Maps is open. Each tile is logged on the companion as
`cloud: z/x/y net|sd <fetch> ms: <bytes> B, ble <transfer> ms`.

# sw-helper

VPS helper for the smartwatch companion: weather, map tiles, routing, and place search
(notifications go from ntfy straight to the companion). Configuration is entirely environment variables. Details:
[docs/server.md](../docs/server.md).

## Container

The container image is defined in the reference deployment (below): a two-stage build that
compiles this directory from GitHub `main` and adds the map tools (osmium, OSRM, Planetiler)
used for on-demand map data.

```bash
curl -fsSLO https://raw.githubusercontent.com/ebrahim37/infra-template/main/vps1/services/rootless/watch/Containerfile
podman build -t localhost/watch-server .
DEVICE_TOKEN="$(openssl rand -base64 32)"   # also enter it on the watch: Settings → Server
# Required variables are set; commented lines show each optional one with its default.
env=(
  -e DEVICE_TOKEN="$DEVICE_TOKEN"
  -e TILE_URL_TEMPLATE='https://tiles.example.com/styles/basic-preview/{z}/{x}/{y}.png'
  -e OSRM_URL_FOOT='https://osrm.example.com/foot'
  -e OSRM_URL_BICYCLE='https://osrm.example.com/bicycle'
  -e OSRM_URL_DRIVING='https://osrm.example.com/driving'
  # Server
  #-e BIND_ADDR=0.0.0.0:8080
  #-e DATA_DIR=/data
  #-e RUST_LOG=info
  #-e USER_AGENT=sw-helper/<version>
  # Weather
  #-e OPEN_METEO_URL=https://api.open-meteo.com/v1/forecast
  #-e WEATHER_CACHE_SECS=900
  # Tiles (TILE_RECOLOR: dark-heuristic for light OSM styles, none if upstream is already dark)
  #-e TILE_RECOLOR=dark-heuristic
  #-e TILE_CACHE_MAX_MB=2048
  #-e 'TILE_ATTRIBUTION=© OpenStreetMap contributors'
  # Routing: one base URL instead of OSRM_URL_*; {profile} -> foot|bicycle|driving
  #-e 'OSRM_URL=https://osrm.example.com/{profile}'
  # Search
  #-e PHOTON_URL=https://photon.komoot.io
  #-e OVERPASS_URL=https://overpass-api.de/api/interpreter
  # On-demand map data (disabled while MAPCACHE_DIR is unset; also mount -v watch-mapcache:/mapcache) using
  # Geofabrik extracts and the areas built from them (~100 km around where the watch is), in a 10 GB ring cache
  #-e MAPCACHE_DIR=/mapcache
  #-e MAPCACHE_MAX_GB=10
  #-e MAP_AREA_KM=100
  #-e GEOFABRIK_INDEX_URL=https://download.geofabrik.de/index-v1.json
  #-e OSRM_PROFILES_DIR=/opt
  #-e PLANETILER_JAR=/opt/planetiler.jar
  #-e JAVA_OPTS=-Xmx3g
)
podman run -d --name watch-server -p 127.0.0.1:47913:8080 -v watch-server-data:/data \
  "${env[@]}" localhost/watch-server
curl -H "Authorization: Bearer $DEVICE_TOKEN" http://127.0.0.1:47913/v1/status
```

Notes:

- The `*_URL` variables must be `https://`. Plain `http://` is only accepted for addresses
  where traffic never crosses the internet: `localhost`/127.0.0.0/8, Tailscale 100.64.0.0/10
  and 169.254.0.0/16 (rootless Podman's `--map-host-loopback` address, used to reach other
  containers' ports on the host). The helper refuses to start otherwise.
- `TILE_URL_TEMPLATE` can't point at `tile.openstreetmap.org`: its usage policy forbids
  the bulk downloads that map sync does.
- The helper serves plain HTTP on port 8080; put a TLS reverse proxy (e.g. Caddy) in front of
  it, because the companion only connects over HTTPS.
- Map tiles and routes come from a tile server (tileserver-gl) and OSRM that you run next to
  the helper; with `MAPCACHE_DIR` set the helper builds their data for the area around the
  watch. The reference deployment below wires this up.

## Reference deployment

Rootless Podman quadlets (helper with map tools, tileserver-gl, OSRM; Caddy in front):
[infra-template/vps1/services/rootless/watch](https://github.com/ebrahim37/infra-template/tree/main/vps1/services/rootless/watch).

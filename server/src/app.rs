//! HTTP API.
//!
//! | Method | Path                              | Auth   | Description                          |
//! |--------|-----------------------------------|--------|--------------------------------------|
//! | GET    | /healthz                          | none   | liveness                             |
//! | GET    | /v1/status                        | bearer | component status                     |
//! | GET    | /v1/weather?lat&lon               | bearer | summarised Open-Meteo forecast       |
//! | GET    | /v1/maps/tile/{z}/{x}/{y}.swt     | bearer | dark 16-colour 256 px SWT tile       |
//! | GET    | /v1/maps/bundle?lat&lon&radius_m&zmin&zmax | bearer | many SWT tiles in one stream (companion background prefetch) |
//! | GET    | /v1/maps/data                     | bearer | on-demand map area status            |
//! | POST   | /v1/maps/data/update `{lat,lon}`  | bearer | re-download + rebuild that area      |
//! | POST   | /v1/maps/data/delete              | bearer | delete all downloaded/built map data |
//! | GET    | /v1/maps/search?q&lat&lon&limit   | bearer | POI/business search (empty q: nearby) |
//! | GET    | /v1/maps/route?from&to&mode       | bearer | simplified route + steps             |

use std::{sync::Arc, time::Duration};

use axum::{
    extract::{Path, Query, Request, State},
    http::{header, HeaderMap, StatusCode},
    middleware::{self, Next},
    response::{IntoResponse, Response},
    routing::{get, post},
    Json, Router,
};
use futures_util::StreamExt;
use serde::Deserialize;
use serde_json::json;
use tower_http::{limit::RequestBodyLimitLayer, timeout::TimeoutLayer, trace::TraceLayer};

use crate::{
    config::Config,
    error::{AppError, AppResult},
    geo,
    mapdata::{Coverage, MapData},
    places::{OsmSearch, PoiSearch},
    routing::{Mode, Osrm, Router as RouteProvider},
    tiles::TileService,
    weather::WeatherService,
};

#[derive(Clone)]
pub struct AppState {
    pub cfg: Arc<Config>,
    pub weather: WeatherService,
    pub tiles: TileService,
    pub mapdata: Option<MapData>,
    pub places: Arc<dyn PoiSearch>,
    pub router: Arc<dyn RouteProvider>,
    /// Routes the local map area cannot answer (see Config::route_fallback_urls).
    pub fallback_router: Option<Arc<dyn RouteProvider>>,
}

impl AppState {
    pub fn new(cfg: Config) -> anyhow::Result<Self> {
        let http = reqwest::Client::builder()
            .user_agent(cfg.user_agent.clone())
            .timeout(Duration::from_secs(20))
            .connect_timeout(Duration::from_secs(10))
            .https_only(false) // https enforced on configured URLs (localhost allowed for tests)
            .build()?;
        Ok(Self {
            weather: WeatherService::new(http.clone(), cfg.open_meteo_url.clone(), cfg.weather_ttl),
            tiles: TileService::new(http.clone(), cfg.tile_url_template.clone(), cfg.tile_recolor, cfg.tile_scale, &cfg.data_dir, cfg.tile_cache_max_bytes),
            mapdata: cfg.mapdata.clone().map(|m| {
                // Extract downloads can take a while; the shared client's 20 s timeout is per request.
                let dl = reqwest::Client::builder().user_agent(cfg.user_agent.clone()).connect_timeout(Duration::from_secs(20)).build();
                MapData::new(m, dl.unwrap_or_else(|_| http.clone()))
            }),
            places: Arc::new(OsmSearch { http: http.clone(), photon_url: cfg.photon_url.clone(), overpass_url: cfg.overpass_url.clone() }),
            fallback_router: {
                let o = Osrm::new(http.clone(), String::new(), cfg.route_fallback_urls.clone(), Some(Duration::from_secs(1)));
                o.configured().then(|| Arc::new(o) as Arc<dyn RouteProvider>)
            },
            router: Arc::new(Osrm::new(http, cfg.osrm_url.clone(), cfg.osrm_profile_urls.clone(), None)),
            cfg: Arc::new(cfg),
        })
    }
}

/// Constant-time comparison so the token cannot be recovered through response timing.
fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

async fn auth(State(st): State<AppState>, headers: HeaderMap, req: Request, next: Next) -> Response {
    let ok = headers
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
        .is_some_and(|t| ct_eq(t.as_bytes(), st.cfg.device_token.as_bytes()));
    if !ok {
        return (StatusCode::UNAUTHORIZED, Json(json!({"error": "unauthorized"}))).into_response();
    }
    next.run(req).await
}

#[derive(Deserialize)]
struct LatLon {
    lat: f64,
    lon: f64,
}

async fn weather(State(st): State<AppState>, Query(q): Query<LatLon>) -> AppResult<impl IntoResponse> {
    Ok(Json(st.weather.get(q.lat, q.lon).await?))
}

/// Cache namespace for a tile, making sure on-demand map data covers it (z >= 10 triggers
/// a build for the area around the tile; lower zooms show whatever area is active).
fn tile_namespace(st: &AppState, z: u8, x: u32, y: u32) -> AppResult<String> {
    let Some(md) = &st.mapdata else { return Ok("default".into()) };
    if z >= 10 {
        let (lat, lon) = geo::tile_center(z, x, y);
        if md.ensure(lat, lon) == Coverage::Preparing {
            return Err(AppError::Preparing);
        }
    }
    md.tile_namespace().ok_or(AppError::Preparing)
}

async fn tile(State(st): State<AppState>, Path((z, x, y)): Path<(u8, u32, String)>) -> AppResult<impl IntoResponse> {
    let y: u32 = y.strip_suffix(".swt").unwrap_or(&y).parse().map_err(|_| AppError::BadRequest("bad y".into()))?;
    if z > 19 || x >= (1u32 << z) || y >= (1u32 << z) {
        return Err(AppError::BadRequest("tile out of range".into()));
    }
    let ns = tile_namespace(&st, z, x, y)?;
    let swt = st.tiles.get(&ns, z, x, y).await?;
    Ok((
        [
            (header::CONTENT_TYPE, "application/vnd.sw-tile".to_string()),
            (header::CACHE_CONTROL, "private, max-age=604800".to_string()),
            (header::HeaderName::from_static("x-attribution"), st.cfg.tile_attribution.clone()),
        ],
        swt,
    ))
}

#[derive(Deserialize)]
struct BundleQ {
    lat: f64,
    lon: f64,
    #[serde(default = "default_radius")]
    radius_m: f64,
    #[serde(default = "default_zmin")]
    zmin: u8,
    #[serde(default = "default_zmax")]
    zmax: u8,
}
fn default_radius() -> f64 {
    3000.0
}
fn default_zmin() -> u8 {
    12
}
fn default_zmax() -> u8 {
    17
}
const MAX_BUNDLE_TILES: usize = 4000;

/// Tiles covering a circle's bounding box, coarse zooms first.
fn bundle_tiles(q: &BundleQ) -> Vec<(u8, u32, u32)> {
    let dlat = q.radius_m / 110_574.0;
    let dlon = q.radius_m / (111_320.0 * q.lat.to_radians().cos().max(0.05));
    let mut out = Vec::new();
    for z in q.zmin..=q.zmax {
        let (x0, y0) = geo::tile_at(q.lat + dlat, q.lon - dlon, z);
        let (x1, y1) = geo::tile_at(q.lat - dlat, q.lon + dlon, z);
        for y in y0..=y1 {
            for x in x0..=x1 {
                out.push((z, x, y));
            }
        }
    }
    out
}

/// Streams many SWT tiles in one response so the companion can fill its SD cache without
/// a request (or TLS handshake) per tile:
/// `"SWB1"` then per tile `z u8 | x u32le | y u32le | len u32le | SWT bytes`, ended by `z = 0xFF`.
async fn bundle(State(st): State<AppState>, Query(q): Query<BundleQ>) -> AppResult<Response> {
    if !(-85.0..=85.0).contains(&q.lat) || !(-180.0..=180.0).contains(&q.lon) || q.zmin > q.zmax || q.zmax > 19 {
        return Err(AppError::BadRequest("bad area".into()));
    }
    if !(100.0..=20_000.0).contains(&q.radius_m) {
        return Err(AppError::BadRequest("radius_m must be 100..20000".into()));
    }
    let ns = match &st.mapdata {
        Some(md) => {
            if md.ensure(q.lat, q.lon) == Coverage::Preparing {
                return Err(AppError::Preparing);
            }
            md.tile_namespace().ok_or(AppError::Preparing)?
        }
        None => "default".into(),
    };
    let list = bundle_tiles(&q);
    if list.len() > MAX_BUNDLE_TILES {
        return Err(AppError::BadRequest(format!("{} tiles requested; reduce radius or zmax", list.len())));
    }
    let tiles = st.tiles.clone();
    // Uncached tiles cost a render each; a few in parallel (order kept) on a multi-core host.
    let records = futures_util::stream::iter(list).map(move |(z, x, y)| {
        let (tiles, ns) = (tiles.clone(), ns.clone());
        async move {
            let Ok(swt) = tiles.get(&ns, z, x, y).await else { return Ok::<_, std::io::Error>(bytes::Bytes::new()) };
            let mut rec = Vec::with_capacity(13 + swt.len());
            rec.push(z);
            rec.extend_from_slice(&x.to_le_bytes());
            rec.extend_from_slice(&y.to_le_bytes());
            rec.extend_from_slice(&(swt.len() as u32).to_le_bytes());
            rec.extend_from_slice(&swt);
            Ok(bytes::Bytes::from(rec))
        }
    })
    .buffered(4);
    let head = futures_util::stream::iter([Ok(bytes::Bytes::from_static(b"SWB1"))]);
    let tail = futures_util::stream::iter([Ok(bytes::Bytes::from_static(&[0xFF]))]);
    let body = axum::body::Body::from_stream(head.chain(records).chain(tail));
    Ok(([(header::CONTENT_TYPE, "application/vnd.sw-tile-bundle")], body).into_response())
}

async fn map_data_status(State(st): State<AppState>) -> AppResult<impl IntoResponse> {
    let md = st.mapdata.as_ref().ok_or(AppError::NotConfigured("MAPCACHE_DIR"))?;
    Ok(Json(md.status()))
}

async fn map_data_update(State(st): State<AppState>, Json(p): Json<LatLon>) -> AppResult<impl IntoResponse> {
    let md = st.mapdata.as_ref().ok_or(AppError::NotConfigured("MAPCACHE_DIR"))?;
    md.refresh(p.lat, p.lon);
    Ok(Json(md.status()))
}

async fn map_data_delete(State(st): State<AppState>) -> AppResult<impl IntoResponse> {
    let md = st.mapdata.as_ref().ok_or(AppError::NotConfigured("MAPCACHE_DIR"))?;
    md.delete_all().await.map_err(|e| AppError::BadRequest(e.to_string()))?;
    let _ = tokio::fs::remove_dir_all(st.cfg.data_dir.join("tiles")).await;
    Ok(Json(md.status()))
}

#[derive(Deserialize)]
struct SearchQ {
    #[serde(default)]
    q: String,
    lat: f64,
    lon: f64,
    #[serde(default = "default_limit")]
    limit: usize,
}
fn default_limit() -> usize {
    10
}

async fn search(State(st): State<AppState>, Query(q): Query<SearchQ>) -> AppResult<impl IntoResponse> {
    if q.q.len() > 100 {
        return Err(AppError::BadRequest("query too long".into()));
    }
    let limit = q.limit.clamp(1, 20);
    // The active area's local index answers in milliseconds; Photon/Overpass cover the rest.
    let local = st.mapdata.as_ref().filter(|md| md.covers(q.lat, q.lon)).and_then(|md| md.pois());
    let mut places = local.map(|idx| idx.search(&q.q, q.lat, q.lon, limit)).unwrap_or_default();
    if places.len() < 3 && (!q.q.trim().is_empty() || places.is_empty()) {
        if let Ok(more) = st.places.search(&q.q, q.lat, q.lon, limit).await {
            for p in more {
                if places.len() >= limit {
                    break;
                }
                if !places.iter().any(|e| e.name == p.name && e.distance_m.abs_diff(p.distance_m) < 200) {
                    places.push(p);
                }
            }
        } else if places.is_empty() {
            return Err(AppError::Upstream("search failed".into()));
        }
    }
    Ok(Json(json!({ "places": places, "attribution": "© OpenStreetMap contributors" })))
}

fn parse_point(s: &str) -> AppResult<(f64, f64)> {
    let (a, b) = s.split_once(',').ok_or_else(|| AppError::BadRequest("point must be lat,lon".into()))?;
    let lat: f64 = a.trim().parse().map_err(|_| AppError::BadRequest("bad lat".into()))?;
    let lon: f64 = b.trim().parse().map_err(|_| AppError::BadRequest("bad lon".into()))?;
    if !(-90.0..=90.0).contains(&lat) || !(-180.0..=180.0).contains(&lon) {
        return Err(AppError::BadRequest("point out of range".into()));
    }
    Ok((lat, lon))
}

#[derive(Deserialize)]
struct RouteQ {
    from: String,
    to: String,
    #[serde(default)]
    mode: Option<String>,
}

async fn route(State(st): State<AppState>, Query(q): Query<RouteQ>) -> AppResult<impl IntoResponse> {
    let mode = Mode::parse(q.mode.as_deref().unwrap_or("walk")).ok_or_else(|| AppError::BadRequest("mode".into()))?;
    let (from, to) = (parse_point(&q.from)?, parse_point(&q.to)?);
    // The local graphs cover ~MAP_AREA_KM around the user; longer trips (or an area still
    // building) go to the fallback servers.
    let local = match &st.mapdata {
        Some(md) => match md.ensure(from.0, from.1) {
            Coverage::Preparing => Err(AppError::Preparing),
            _ if !md.covers(to.0, to.1) => Err(AppError::BadRequest("destination is outside the current map area".into())),
            _ => Ok(()),
        },
        None => Ok(()),
    };
    if local.is_ok() {
        let r = st.router.route(from, to, mode).await?;
        if r.status == "ok" || st.fallback_router.is_none() {
            return Ok(Json(r));
        }
    }
    match &st.fallback_router {
        Some(fb) => Ok(Json(fb.route(from, to, mode).await?)),
        None => Err(local.err().unwrap_or(AppError::Upstream("no route".into()))),
    }
}

async fn status(State(st): State<AppState>) -> AppResult<impl IntoResponse> {
    Ok(Json(json!({
        "version": env!("CARGO_PKG_VERSION"),
        "weather": !st.cfg.open_meteo_url.is_empty(),
        "tiles": !st.cfg.tile_url_template.is_empty(),
        "routing": !st.cfg.osrm_url.is_empty() || st.cfg.osrm_profile_urls.iter().any(|u| !u.is_empty()),
        "search": !st.cfg.photon_url.is_empty(),
        // Names the companion's SD tile cache: tiles of another style must not be mixed in.
        "tile_style": if st.cfg.tile_scale == 2 { "2x" } else { "1x" },
        "map_data": st.mapdata.as_ref().map(|m| m.status()),
    })))
}

pub fn router(st: AppState) -> Router {
    let v1 = Router::new()
        .route("/status", get(status))
        .route("/weather", get(weather))
        .route("/maps/tile/{z}/{x}/{y}", get(tile))
        .route("/maps/search", get(search))
        .route("/maps/route", get(route))
        .route("/maps/bundle", get(bundle))
        .route("/maps/data", get(map_data_status))
        .route("/maps/data/update", post(map_data_update))
        .route("/maps/data/delete", post(map_data_delete))
        .route_layer(middleware::from_fn_with_state(st.clone(), auth));
    Router::new()
        .route("/healthz", get(|| async { "ok" }))
        .nest("/v1", v1)
        .layer(RequestBodyLimitLayer::new(16 * 1024))
        .layer(TimeoutLayer::with_status_code(StatusCode::GATEWAY_TIMEOUT, Duration::from_secs(30)))
        .layer(TraceLayer::new_for_http())
        .with_state(st)
}

#[cfg(test)]
mod tests {
    use super::ct_eq;

    #[test]
    fn constant_time_compare() {
        assert!(ct_eq(b"abc", b"abc"));
        assert!(!ct_eq(b"abc", b"abd"));
        assert!(!ct_eq(b"abc", b"abcd"));
    }
}

//! Configuration from environment variables. Secrets never leave the server.

use std::{net::SocketAddr, path::PathBuf, time::Duration};

use anyhow::{bail, Context, Result};

#[derive(Clone, Debug)]
pub struct Config {
    pub bind: SocketAddr,
    /// Bearer token the companion presents. Required.
    pub device_token: String,
    pub data_dir: PathBuf,
    pub open_meteo_url: String,
    pub weather_ttl: Duration,
    /// e.g. `https://tiles.example.com/styles/osm-bright/{z}/{x}/{y}.png` (self-hosted or a
    /// provider whose terms allow caching). Empty disables tiles.
    pub tile_url_template: String,
    pub tile_recolor: TileRecolor,
    pub tile_cache_max_bytes: u64,
    pub tile_attribution: String,
    pub photon_url: String,
    pub overpass_url: String,
    /// OSRM base URL; `{profile}` is replaced by foot/bicycle/driving if present.
    pub osrm_url: String,
    /// Per-profile overrides (OSRM_URL_FOOT / _BICYCLE / _DRIVING), for one osrm-routed per profile.
    pub osrm_profile_urls: [String; 3],
    /// On-demand map data (see mapdata.rs); None when MAPCACHE_DIR is unset.
    pub mapdata: Option<crate::mapdata::MapDataConfig>,
    pub user_agent: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TileRecolor {
    /// Heuristic dark palette for light OSM-Carto-like raster styles.
    DarkHeuristic,
    /// Upstream already dark (e.g. a self-hosted dark vector style): quantize only.
    None,
}

fn var(name: &str) -> Option<String> {
    std::env::var(name).ok().map(|v| v.trim().to_string()).filter(|v| !v.is_empty())
}

/// Plain http is accepted only where the transport is already private: loopback,
/// Tailscale/Headscale addresses (100.64.0.0/10), which travel inside WireGuard, and
/// link-local 169.254.0.0/16 (the rootless Podman host-loopback mapping between containers).
fn plain_http_allowed(url: &str) -> bool {
    let Some(rest) = url.strip_prefix("http://") else { return false };
    let host = rest.split(['/', ':']).next().unwrap_or("");
    if host == "localhost" {
        return true;
    }
    match host.parse::<std::net::Ipv4Addr>() {
        Ok(ip) => ip.is_loopback() || ip.is_link_local() || (ip.octets()[0] == 100 && (ip.octets()[1] & 0xC0) == 64),
        Err(_) => false,
    }
}

fn https_or_empty(name: &str, v: String) -> Result<String> {
    if !v.is_empty() && !v.starts_with("https://") && !plain_http_allowed(&v) {
        bail!("{name} must use https:// (plain http is only accepted for localhost and tailnet upstreams)");
    }
    Ok(v.trim_end_matches('/').to_string())
}

impl Config {
    pub fn from_env() -> Result<Self> {
        let device_token = var("DEVICE_TOKEN").context("DEVICE_TOKEN is required")?;
        if device_token.len() < 24 {
            bail!("DEVICE_TOKEN must be at least 24 characters");
        }
        let tile_url_template = https_or_empty("TILE_URL_TEMPLATE", var("TILE_URL_TEMPLATE").unwrap_or_default())?;
        // Bulk access to the volunteer-run OSM tile servers violates their usage policy.
        if tile_url_template.contains("tile.openstreetmap.org") {
            bail!("TILE_URL_TEMPLATE must not point at tile.openstreetmap.org; self-host tiles or use a provider that permits caching");
        }
        Ok(Self {
            bind: var("BIND_ADDR").unwrap_or_else(|| "0.0.0.0:8080".into()).parse().context("BIND_ADDR")?,
            device_token,
            data_dir: var("DATA_DIR").unwrap_or_else(|| "./data".into()).into(),
            open_meteo_url: https_or_empty(
                "OPEN_METEO_URL",
                var("OPEN_METEO_URL").unwrap_or_else(|| "https://api.open-meteo.com/v1/forecast".into()),
            )?,
            weather_ttl: Duration::from_secs(var("WEATHER_CACHE_SECS").and_then(|v| v.parse().ok()).unwrap_or(900)),
            tile_url_template,
            tile_recolor: match var("TILE_RECOLOR").as_deref() {
                Some("none") => TileRecolor::None,
                _ => TileRecolor::DarkHeuristic,
            },
            tile_cache_max_bytes: var("TILE_CACHE_MAX_MB").and_then(|v| v.parse::<u64>().ok()).unwrap_or(2048) * 1024 * 1024,
            tile_attribution: var("TILE_ATTRIBUTION").unwrap_or_else(|| "© OpenStreetMap contributors".into()),
            photon_url: https_or_empty("PHOTON_URL", var("PHOTON_URL").unwrap_or_else(|| "https://photon.komoot.io".into()))?,
            overpass_url: https_or_empty(
                "OVERPASS_URL",
                var("OVERPASS_URL").unwrap_or_else(|| "https://overpass-api.de/api/interpreter".into()),
            )?,
            osrm_url: https_or_empty("OSRM_URL", var("OSRM_URL").unwrap_or_default())?,
            osrm_profile_urls: [
                https_or_empty("OSRM_URL_FOOT", var("OSRM_URL_FOOT").unwrap_or_default())?,
                https_or_empty("OSRM_URL_BICYCLE", var("OSRM_URL_BICYCLE").unwrap_or_default())?,
                https_or_empty("OSRM_URL_DRIVING", var("OSRM_URL_DRIVING").unwrap_or_default())?,
            ],
            mapdata: var("MAPCACHE_DIR").map(|dir| crate::mapdata::MapDataConfig {
                dir: dir.into(),
                max_bytes: var("MAPCACHE_MAX_GB").and_then(|v| v.parse::<u64>().ok()).unwrap_or(10) * 1024 * 1024 * 1024,
                area_km: var("MAP_AREA_KM").and_then(|v| v.parse().ok()).unwrap_or(100.0),
                index_url: var("GEOFABRIK_INDEX_URL").unwrap_or_else(|| "https://download.geofabrik.de/index-v1.json".into()),
                osrm_profiles_dir: var("OSRM_PROFILES_DIR").unwrap_or_else(|| "/opt".into()).into(),
                planetiler_jar: var("PLANETILER_JAR").unwrap_or_else(|| "/opt/planetiler.jar".into()).into(),
                java_opts: var("JAVA_OPTS").map(|o| o.split_whitespace().map(String::from).collect()).unwrap_or_default(),
            }),
            user_agent: var("USER_AGENT").unwrap_or_else(|| concat!("sw-helper/", env!("CARGO_PKG_VERSION")).into()),
        })
    }

    /// Minimal configuration for tests.
    pub fn for_tests(data_dir: PathBuf) -> Self {
        Self {
            bind: "127.0.0.1:0".parse().unwrap(),
            device_token: "test-token-0123456789abcdef".into(),
            data_dir,
            open_meteo_url: String::new(),
            weather_ttl: Duration::from_secs(900),
            tile_url_template: String::new(),
            tile_recolor: TileRecolor::DarkHeuristic,
            tile_cache_max_bytes: 64 * 1024 * 1024,
            tile_attribution: "© OpenStreetMap contributors".into(),
            photon_url: String::new(),
            overpass_url: String::new(),
            osrm_url: String::new(),
            osrm_profile_urls: Default::default(),
            mapdata: None,
            user_agent: "sw-helper-test".into(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_http_only_for_loopback_and_tailnet() {
        assert!(plain_http_allowed("http://127.0.0.1:2586"));
        assert!(plain_http_allowed("http://localhost/x"));
        assert!(plain_http_allowed("http://100.64.0.1:2586"));
        assert!(plain_http_allowed("http://100.127.255.254"));
        assert!(!plain_http_allowed("http://100.128.0.1"));
        assert!(!plain_http_allowed("http://100.63.0.1"));
        assert!(!plain_http_allowed("http://tiles.example.com"));
        assert!(!plain_http_allowed("http://100.64.0.1.evil.com"));
        assert!(plain_http_allowed("http://169.254.1.3:47914/x"));
        assert!(https_or_empty("X", "http://192.168.1.2".into()).is_err());
        assert_eq!(https_or_empty("X", "https://a.b/".into()).unwrap(), "https://a.b");
    }
}

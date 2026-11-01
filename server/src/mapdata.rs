//! On-demand map data: when a tile or route is requested outside the current map area, the
//! helper picks the smallest Geofabrik extract containing it, downloads that extract into a
//! size-bounded ring cache, clips ~`MAP_AREA_KM` around the point, and builds OSRM routing
//! graphs (foot, bicycle, car) and OpenMapTiles vector tiles (Planetiler) for the clip.
//!
//! Cache layout (`MAPCACHE_DIR`, shared read-only with the tile and OSRM containers):
//!
//! ```text
//! extracts/<geofabrik-id>.osm.pbf     downloaded extracts            (evictable)
//! areas/<area-id>/                    region.osm.pbf, tiles.mbtiles,  (evictable, not the active one)
//!                                     pois.json (search index),
//!                                     osrm/{foot,bicycle,car}/region.osrm*
//! sources/                            Planetiler water/Natural Earth inputs (kept)
//! active -> areas/<area-id>           switched atomically; the servers reload when it changes
//! ```
//!
//! The cache is a ring: before anything new is written, the oldest extracts and areas (by
//! modification time) are deleted until the new data fits under `MAPCACHE_MAX_GB`.

use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{Duration, SystemTime},
};

use anyhow::{bail, Context, Result};
use futures_util::StreamExt;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use tokio::{io::AsyncWriteExt, process::Command};

use crate::poi::PoiIndex;

const INDEX_MAX_AGE: Duration = Duration::from_secs(7 * 24 * 3600);
const PROFILES: [&str; 3] = ["foot", "bicycle", "car"];

#[derive(Clone, Debug)]
pub struct MapDataConfig {
    pub dir: PathBuf,
    pub max_bytes: u64,
    /// Width of the clipped square, km.
    pub area_km: f64,
    pub index_url: String,
    pub osrm_profiles_dir: PathBuf,
    pub planetiler_jar: PathBuf,
    pub java_opts: Vec<String>,
}

/// Bounding box: min lon, min lat, max lon, max lat.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct BBox(pub [f64; 4]);

impl BBox {
    pub fn around(lat: f64, lon: f64, width_km: f64) -> Self {
        let dlat = width_km / 2.0 / 110.574;
        let dlon = width_km / 2.0 / (111.320 * lat.to_radians().cos().max(0.05));
        BBox([(lon - dlon).max(-180.0), (lat - dlat).max(-85.0), (lon + dlon).min(180.0), (lat + dlat).min(85.0)])
    }
    pub fn contains(&self, lat: f64, lon: f64) -> bool {
        lon >= self.0[0] && lon <= self.0[2] && lat >= self.0[1] && lat <= self.0[3]
    }
    /// The box shrunk by `frac` of its size on each side.
    pub fn inner(&self, frac: f64) -> Self {
        let (w, h) = (self.0[2] - self.0[0], self.0[3] - self.0[1]);
        BBox([self.0[0] + w * frac, self.0[1] + h * frac, self.0[2] - w * frac, self.0[3] - h * frac])
    }
    fn corners(&self) -> [(f64, f64); 4] {
        let [a, b, c, d] = self.0;
        [(b, a), (b, c), (d, a), (d, c)]
    }
    fn arg(&self) -> String {
        format!("{:.5},{:.5},{:.5},{:.5}", self.0[0], self.0[1], self.0[2], self.0[3])
    }
}

/// Area ids snap the centre to a grid (0.1° for areas of 50 km and up, 0.01° below) so
/// nearby requests share a build while the point stays well inside the box.
pub fn area_for(lat: f64, lon: f64, width_km: f64) -> (String, BBox) {
    let steps = if width_km >= 50.0 { 10.0 } else { 100.0 };
    let (clat, clon) = ((lat * steps).round() / steps, (lon * steps).round() / steps);
    (format!("{clat:+.2}_{clon:+.2}_{}km", width_km.round() as u32), BBox::around(clat, clon, width_km))
}

// ---- Geofabrik index ----------------------------------------------------------------------

#[derive(Clone, Debug)]
pub struct Region {
    pub id: String,
    pub pbf_url: String,
    polygons: Vec<Vec<Vec<(f64, f64)>>>, // polygons -> rings -> (lon, lat)
    area: f64,
}

fn ring_contains(ring: &[(f64, f64)], lon: f64, lat: f64) -> bool {
    let mut inside = false;
    let mut j = ring.len().wrapping_sub(1);
    for i in 0..ring.len() {
        let (xi, yi) = ring[i];
        let (xj, yj) = ring[j];
        if (yi > lat) != (yj > lat) && lon < (xj - xi) * (lat - yi) / (yj - yi) + xi {
            inside = !inside;
        }
        j = i;
    }
    inside
}

impl Region {
    pub fn contains(&self, lat: f64, lon: f64) -> bool {
        self.polygons.iter().any(|rings| match rings.split_first() {
            Some((outer, holes)) => ring_contains(outer, lon, lat) && !holes.iter().any(|h| ring_contains(h, lon, lat)),
            None => false,
        })
    }
}

pub fn parse_index(v: &Value) -> Vec<Region> {
    let mut out = Vec::new();
    for f in v["features"].as_array().into_iter().flatten() {
        let (Some(id), Some(url)) = (f["properties"]["id"].as_str(), f["properties"]["urls"]["pbf"].as_str()) else { continue };
        let coords = &f["geometry"]["coordinates"];
        let polys: Vec<&Value> = match f["geometry"]["type"].as_str() {
            Some("Polygon") => vec![coords],
            Some("MultiPolygon") => coords.as_array().map(|a| a.iter().collect()).unwrap_or_default(),
            _ => continue,
        };
        let polygons: Vec<Vec<Vec<(f64, f64)>>> = polys
            .iter()
            .map(|p| {
                p.as_array()
                    .into_iter()
                    .flatten()
                    .map(|ring| {
                        ring.as_array().into_iter().flatten().filter_map(|pt| Some((pt.get(0)?.as_f64()?, pt.get(1)?.as_f64()?))).collect()
                    })
                    .collect()
            })
            .collect();
        // Bounding-box area as a size proxy: smaller = more specific extract.
        let (mut x0, mut y0, mut x1, mut y1) = (f64::MAX, f64::MAX, f64::MIN, f64::MIN);
        for (x, y) in polygons.iter().flat_map(|p| p.first()).flatten() {
            x0 = x0.min(*x);
            y0 = y0.min(*y);
            x1 = x1.max(*x);
            y1 = y1.max(*y);
        }
        if x1 < x0 {
            continue;
        }
        out.push(Region { id: id.to_string(), pbf_url: url.to_string(), polygons, area: (x1 - x0) * (y1 - y0) });
    }
    out
}

/// Smallest extract containing the whole box, else the smallest containing its centre.
pub fn pick_region<'a>(regions: &'a [Region], bbox: &BBox) -> Option<&'a Region> {
    let smallest = |it: Vec<&'a Region>| it.into_iter().min_by(|a, b| a.area.total_cmp(&b.area));
    let whole: Vec<&Region> = regions.iter().filter(|r| bbox.corners().iter().all(|&(lat, lon)| r.contains(lat, lon))).collect();
    if !whole.is_empty() {
        return smallest(whole);
    }
    let (clat, clon) = ((bbox.0[1] + bbox.0[3]) / 2.0, (bbox.0[0] + bbox.0[2]) / 2.0);
    smallest(regions.iter().filter(|r| r.contains(clat, clon)).collect())
}

// ---- Ring cache ---------------------------------------------------------------------------

fn dir_size(p: &Path) -> u64 {
    let Ok(m) = std::fs::symlink_metadata(p) else { return 0 };
    if !m.is_dir() {
        return m.len();
    }
    std::fs::read_dir(p).map(|rd| rd.flatten().map(|e| dir_size(&e.path())).sum()).unwrap_or(0)
}

/// Deletes the oldest evictable entries (files in `extracts/`, directories in `areas/`)
/// until the cache plus `incoming` bytes fits in `max_bytes`. `keep` entries are never
/// deleted (the active area and anything being built). Returns bytes freed.
pub fn make_room(root: &Path, max_bytes: u64, incoming: u64, keep: &[PathBuf]) -> u64 {
    let mut total = dir_size(root);
    let mut entries: Vec<(SystemTime, u64, PathBuf)> = Vec::new();
    for sub in ["extracts", "areas"] {
        for e in std::fs::read_dir(root.join(sub)).into_iter().flatten().flatten() {
            let p = e.path();
            if keep.iter().any(|k| k == &p) {
                continue;
            }
            let mtime = e.metadata().and_then(|m| m.modified()).unwrap_or(SystemTime::UNIX_EPOCH);
            entries.push((mtime, dir_size(&p), p));
        }
    }
    entries.sort_by_key(|e| e.0);
    let mut freed = 0;
    for (_, len, p) in entries {
        if total + incoming <= max_bytes {
            break;
        }
        let ok = if p.is_dir() { std::fs::remove_dir_all(&p) } else { std::fs::remove_file(&p) };
        if ok.is_ok() {
            tracing::info!("map cache: evicted {} ({} MB)", p.display(), len / 1_048_576);
            total = total.saturating_sub(len);
            freed += len;
        }
    }
    freed
}

// ---- Service ------------------------------------------------------------------------------

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Area {
    pub id: String,
    pub bbox: BBox,
    pub region: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Coverage {
    Ready,
    Preparing,
}

#[derive(Default)]
struct State {
    active: Option<Area>,
    building: Option<Area>,
    last_error: Option<String>,
    /// Search index of the active area (area id), loaded or built in the background.
    pois: Option<(String, Arc<PoiIndex>)>,
    pois_loading: bool,
}

/// Extracts named POIs from an area clip into `pois.json` (osmium tags-filter + export).
async fn build_pois(dir: &Path) -> Result<PoiIndex> {
    let tmp = dir.join("pois-work");
    tokio::fs::create_dir_all(&tmp).await?;
    let filtered = tmp.join("pois.osm.pbf");
    let seq = tmp.join("pois.geojsonseq");
    run(Command::new("osmium")
        .args(["tags-filter", "--overwrite", "-o"])
        .arg(&filtered)
        .arg(dir.join("region.osm.pbf"))
        .args(["nwr/amenity", "nwr/shop", "nwr/tourism", "nwr/leisure", "nwr/office", "nwr/craft", "nwr/healthcare", "nwr/historic"])
        .args([
            "nwr/railway=station,halt",
            "nwr/public_transport=station",
            "nwr/aeroway=aerodrome",
            "n/place",
            "wr/building=train_station,hospital,stadium,university,school,church",
        ]))
    .await?;
    run(Command::new("osmium").args(["export", "--overwrite", "-f", "geojsonseq", "-o"]).arg(&seq).arg(&filtered)).await?;
    let (seq2, out) = (seq.clone(), dir.join("pois.json"));
    let idx = tokio::task::spawn_blocking(move || -> Result<PoiIndex> {
        let idx = PoiIndex::from_geojsonseq(&std::fs::read_to_string(&seq2)?);
        idx.save(&out)?;
        Ok(idx)
    })
    .await??;
    tokio::fs::remove_dir_all(&tmp).await.ok();
    Ok(idx)
}

#[derive(Clone)]
pub struct MapData {
    cfg: Arc<MapDataConfig>,
    http: reqwest::Client,
    state: Arc<Mutex<State>>,
}

impl MapData {
    pub fn new(cfg: MapDataConfig, http: reqwest::Client) -> Self {
        let mut st = State::default();
        // Resume the previously active area after a restart.
        if let Ok(target) = std::fs::read_link(cfg.dir.join("active")) {
            if let Ok(b) = std::fs::read(cfg.dir.join(&target).join("area.json")) {
                st.active = serde_json::from_slice(&b).ok();
            }
        }
        if let Some(a) = &st.active {
            tracing::info!("map area {} active ({})", a.id, a.region);
        }
        Self { cfg: Arc::new(cfg), http, state: Arc::new(Mutex::new(st)) }
    }

    pub fn active(&self) -> Option<Area> {
        self.state.lock().unwrap().active.clone()
    }

    /// Cache namespace for rendered tiles, so a new area never serves tiles cached from an
    /// older one.
    pub fn tile_namespace(&self) -> Option<String> {
        self.active().map(|a| a.id)
    }

    pub fn status(&self) -> Value {
        let st = self.state.lock().unwrap();
        serde_json::json!({
            "active": st.active.as_ref().map(|a| &a.id),
            "building": st.building.as_ref().map(|a| &a.id),
            "last_error": st.last_error,
        })
    }

    /// Ready if the active area covers the point with some margin; otherwise starts (or
    /// joins) a build for the area around it.
    pub fn ensure(&self, lat: f64, lon: f64) -> Coverage {
        self.start(lat, lon, false)
    }

    /// Re-downloads the extract and rebuilds the area around the point (manual update).
    /// The current area keeps serving until the new build replaces it.
    pub fn refresh(&self, lat: f64, lon: f64) -> Coverage {
        self.start(lat, lon, true)
    }

    fn start(&self, lat: f64, lon: f64, force: bool) -> Coverage {
        let mut st = self.state.lock().unwrap();
        if !force && st.active.as_ref().is_some_and(|a| a.bbox.inner(0.15).contains(lat, lon)) {
            return Coverage::Ready;
        }
        if st.building.is_some() {
            return Coverage::Preparing; // one build at a time; the next request starts the next one
        }
        let (id, bbox) = area_for(lat, lon, self.cfg.area_km);
        st.building = Some(Area { id: id.clone(), bbox, region: String::new() });
        drop(st);
        let me = self.clone();
        tokio::spawn(async move {
            let res = me.build(&id, bbox, force).await;
            let mut st = me.state.lock().unwrap();
            st.building = None;
            match res {
                Ok(area) => {
                    tracing::info!("map area {} ready", area.id);
                    st.active = Some(area);
                    st.last_error = None;
                }
                Err(e) => {
                    tracing::warn!("map area {id} failed: {e:#}");
                    st.last_error = Some(format!("{e:#}"));
                }
            }
        });
        Coverage::Preparing
    }

    /// The active area's POI index; None (and a background load or build starts) until ready.
    pub fn pois(&self) -> Option<Arc<PoiIndex>> {
        let mut st = self.state.lock().unwrap();
        let area = st.active.clone()?;
        if let Some((id, idx)) = &st.pois {
            if *id == area.id {
                return Some(idx.clone());
            }
        }
        if st.pois_loading {
            return None;
        }
        st.pois_loading = true;
        drop(st);
        let me = self.clone();
        tokio::spawn(async move {
            let dir = me.cfg.dir.join("areas").join(&area.id);
            let path = dir.join("pois.json");
            let res = match tokio::task::spawn_blocking(move || PoiIndex::load(&path)).await {
                Ok(Ok(idx)) => Ok(idx),
                _ => build_pois(&dir).await, // areas built before the index existed
            };
            let mut st = me.state.lock().unwrap();
            st.pois_loading = false;
            match res {
                Ok(idx) => {
                    tracing::info!("search index for {}: {} places", area.id, idx.len());
                    st.pois = Some((area.id, Arc::new(idx)));
                }
                Err(e) => tracing::warn!("search index for {}: {e:#}", area.id),
            }
        });
        None
    }

    /// True if the point lies inside the active area at all (no margin).
    pub fn covers(&self, lat: f64, lon: f64) -> bool {
        self.active().is_some_and(|a| a.bbox.contains(lat, lon))
    }

    fn keep(&self) -> Vec<PathBuf> {
        let st = self.state.lock().unwrap();
        st.active.iter().chain(st.building.iter()).map(|a| self.cfg.dir.join("areas").join(&a.id)).collect()
    }

    async fn regions(&self) -> Result<Vec<Region>> {
        let path = self.cfg.dir.join("geofabrik-index.json");
        let fresh = std::fs::metadata(&path).and_then(|m| m.modified()).is_ok_and(|t| t.elapsed().unwrap_or_default() < INDEX_MAX_AGE);
        if !fresh {
            let body = self.http.get(&self.cfg.index_url).send().await?.error_for_status()?.bytes().await?;
            tokio::fs::write(path.with_extension("tmp"), &body).await?;
            tokio::fs::rename(path.with_extension("tmp"), &path).await?;
        }
        let bytes = tokio::fs::read(&path).await?;
        Ok(tokio::task::spawn_blocking(move || serde_json::from_slice::<Value>(&bytes).map(|v| parse_index(&v))).await??)
    }

    async fn download(&self, url: &str, dest: &Path) -> Result<()> {
        let resp = self.http.get(url).timeout(Duration::from_secs(4 * 3600)).send().await?.error_for_status()?;
        let len = resp.content_length().unwrap_or(0);
        let (root, max, keep) = (self.cfg.dir.clone(), self.cfg.max_bytes, self.keep());
        tokio::task::spawn_blocking(move || make_room(&root, max, len, &keep)).await?;
        let part = dest.with_extension("part");
        let mut f = tokio::fs::File::create(&part).await?;
        let mut stream = resp.bytes_stream();
        while let Some(chunk) = stream.next().await {
            f.write_all(&chunk?).await?;
        }
        f.flush().await?;
        drop(f);
        tokio::fs::rename(&part, dest).await?;
        Ok(())
    }

    /// Deletes all downloaded and built map data (the tile and OSRM servers go idle until the
    /// next request builds an area again). Refused while a build runs.
    pub async fn delete_all(&self) -> Result<()> {
        {
            let mut st = self.state.lock().unwrap();
            if st.building.is_some() {
                bail!("a map build is running; try again when it finishes");
            }
            st.active = None;
            st.last_error = None;
        }
        let root = self.cfg.dir.clone();
        let _ = tokio::fs::remove_file(root.join("active")).await;
        for d in ["areas", "extracts", "sources", "tmp"] {
            let _ = tokio::fs::remove_dir_all(root.join(d)).await;
        }
        let _ = tokio::fs::remove_file(root.join("geofabrik-index.json")).await;
        tracing::info!("map cache cleared");
        Ok(())
    }

    async fn build(&self, id: &str, bbox: BBox, force: bool) -> Result<Area> {
        let root = &self.cfg.dir;
        for d in ["extracts", "areas", "sources", "tmp"] {
            tokio::fs::create_dir_all(root.join(d)).await?;
        }
        let area_dir = root.join("areas").join(id);
        if let (false, Ok(b)) = (force, tokio::fs::read(area_dir.join("area.json")).await) {
            // Built earlier and still cached: just switch to it.
            let area: Area = serde_json::from_slice(&b)?;
            self.activate(&area).await?;
            return Ok(area);
        }
        let regions = self.regions().await.context("Geofabrik index")?;
        let region = pick_region(&regions, &bbox).context("no Geofabrik extract covers this location")?.clone();
        let extract = root.join("extracts").join(format!("{}.osm.pbf", region.id.replace('/', "_")));
        if force {
            let _ = tokio::fs::remove_file(&extract).await; // fetch the latest Geofabrik extract
        }
        if !extract.exists() {
            tracing::info!("downloading Geofabrik extract {} for area {id}", region.id);
            self.download(&region.pbf_url, &extract).await.with_context(|| format!("download {}", region.pbf_url))?;
        }
        filetime_touch(&extract);

        let work = root.join("tmp").join(id);
        let _ = tokio::fs::remove_dir_all(&work).await;
        tokio::fs::create_dir_all(&work).await?;
        let clip = work.join("region.osm.pbf");
        tracing::info!("clipping {} to {}", region.id, bbox.arg());
        run(Command::new("osmium")
            .args(["extract", "--strategy=smart", "--overwrite", "--bbox"])
            .arg(bbox.arg())
            .arg("-o")
            .arg(&clip)
            .arg(&extract))
        .await?;
        // Room for the derived data (roughly 8x the clipped extract across all outputs).
        let est = std::fs::metadata(&clip).map(|m| m.len() * 8).unwrap_or(0);
        let (r, max, mut keep) = (root.clone(), self.cfg.max_bytes, self.keep());
        keep.push(extract.clone());
        tokio::task::spawn_blocking(move || make_room(&r, max, est, &keep)).await?;

        for p in PROFILES {
            tracing::info!("building OSRM graph ({p}) for {id}");
            let dir = work.join("osrm").join(p);
            tokio::fs::create_dir_all(&dir).await?;
            tokio::fs::copy(&clip, dir.join("region.osm.pbf")).await?;
            let osrm = dir.join("region.osrm");
            run(Command::new("osrm-extract")
                .arg("-p")
                .arg(self.cfg.osrm_profiles_dir.join(format!("{p}.lua")))
                .arg(dir.join("region.osm.pbf")))
            .await
            .with_context(|| format!("no routable {p} ways around this location (open water?)"))?;
            run(Command::new("osrm-partition").arg(&osrm)).await?;
            run(Command::new("osrm-customize").arg(&osrm)).await?;
            tokio::fs::remove_file(dir.join("region.osm.pbf")).await.ok();
        }

        tracing::info!("building search index for {id}");
        if let Err(e) = build_pois(&work).await {
            tracing::warn!("search index for {id}: {e:#}"); // search falls back to Photon
        }

        tracing::info!("building vector tiles for {id}");
        run(Command::new("java")
            .args(&self.cfg.java_opts)
            .arg("-jar")
            .arg(&self.cfg.planetiler_jar)
            .arg(format!("--osm-path={}", clip.display()))
            .arg(format!("--bounds={}", bbox.arg()))
            .arg("--download")
            .arg(format!("--download-dir={}", root.join("sources").display()))
            .arg(format!("--tmpdir={}", work.join("planetiler").display()))
            .arg(format!("--output={}", work.join("tiles.mbtiles").display()))
            .arg("--force"))
        .await?;
        tokio::fs::remove_dir_all(work.join("planetiler")).await.ok();

        let area = Area { id: id.to_string(), bbox, region: region.id.clone() };
        tokio::fs::write(work.join("area.json"), serde_json::to_vec(&area)?).await?;
        let _ = tokio::fs::remove_dir_all(&area_dir).await;
        tokio::fs::rename(&work, &area_dir).await?;
        self.activate(&area).await?;
        let (r, max, keep) = (root.clone(), self.cfg.max_bytes, vec![area_dir]);
        tokio::task::spawn_blocking(move || make_room(&r, max, 0, &keep)).await?;
        Ok(area)
    }

    /// Points `active` at the area; the tile and OSRM containers watch this link.
    async fn activate(&self, area: &Area) -> Result<()> {
        let root = &self.cfg.dir;
        let tmp = root.join("active.tmp");
        let _ = tokio::fs::remove_file(&tmp).await;
        tokio::fs::symlink(Path::new("areas").join(&area.id), &tmp).await?;
        tokio::fs::rename(&tmp, root.join("active")).await?;
        filetime_touch(&root.join("areas").join(&area.id));
        self.state.lock().unwrap().active = Some(area.clone());
        Ok(())
    }
}

/// Marks an entry as recently used so the ring evicts it last.
fn filetime_touch(p: &Path) {
    if let Ok(f) = std::fs::File::options().read(true).open(p) {
        let _ = f.set_modified(SystemTime::now());
    }
}

async fn run(cmd: &mut Command) -> Result<()> {
    let prog = format!("{:?}", cmd.as_std().get_program());
    let status = cmd.kill_on_drop(true).status().await.with_context(|| format!("spawn {prog}"))?;
    if !status.success() {
        bail!("{prog} failed: {status}");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn square(id: &str, x0: f64, y0: f64, x1: f64, y1: f64) -> Value {
        json!({"properties": {"id": id, "urls": {"pbf": format!("https://dl/{id}.osm.pbf")}},
               "geometry": {"type": "MultiPolygon", "coordinates": [[[[x0, y0], [x1, y0], [x1, y1], [x0, y1], [x0, y0]]]]}})
    }

    #[test]
    fn picks_smallest_region_covering_the_area() {
        let idx = json!({"features": [
            square("north-america", -170.0, 5.0, -50.0, 85.0),
            square("canada", -141.0, 41.0, -52.0, 84.0),
            square("ontario", -95.2, 41.6, -74.8, 56.9),
            square("quebec", -74.8, 44.9, -57.1, 62.6),
        ]});
        let regions = parse_index(&idx);
        assert_eq!(regions.len(), 4);
        let (_, toronto) = area_for(43.78, -79.41, 100.0);
        assert_eq!(pick_region(&regions, &toronto).unwrap().id, "ontario");
        // Straddling Ontario/Quebec: no province holds the whole box, Canada does.
        let ottawa = BBox::around(45.42, -75.0, 100.0);
        assert_eq!(pick_region(&regions, &ottawa).unwrap().id, "canada");
        assert!(pick_region(&regions, &BBox::around(-30.0, 20.0, 50.0)).is_none());
    }

    #[test]
    fn holes_are_excluded() {
        let idx = json!({"features": [{"properties": {"id": "ring", "urls": {"pbf": "u"}},
            "geometry": {"type": "Polygon", "coordinates": [
                [[0.0, 0.0], [10.0, 0.0], [10.0, 10.0], [0.0, 10.0], [0.0, 0.0]],
                [[4.0, 4.0], [6.0, 4.0], [6.0, 6.0], [4.0, 6.0], [4.0, 4.0]]]}}]});
        let r = &parse_index(&idx)[0];
        assert!(r.contains(1.0, 1.0));
        assert!(!r.contains(5.0, 5.0));
        assert!(!r.contains(11.0, 1.0));
    }

    #[test]
    fn areas_snap_and_cover_their_neighbourhood() {
        let (a, bbox) = area_for(43.7801, -79.4119, 100.0);
        let (b, _) = area_for(43.81, -79.38, 100.0);
        assert_eq!(a, b);
        assert_eq!(a, "+43.80_-79.40_100km");
        let (small, _) = area_for(43.7384, 7.4246, 10.0);
        assert_eq!(small, "+43.74_+7.42_10km");
        assert!(bbox.inner(0.15).contains(43.78, -79.41));
        let w_km = (bbox.0[2] - bbox.0[0]) * 111.32 * 43.8f64.to_radians().cos();
        assert!((w_km - 100.0).abs() < 1.0);
    }

    #[test]
    fn ring_evicts_oldest_first_and_keeps_protected() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path();
        std::fs::create_dir_all(root.join("extracts")).unwrap();
        std::fs::create_dir_all(root.join("areas/active")).unwrap();
        let write = |p: &str, n: usize, age_s: u64| {
            let path = root.join(p);
            std::fs::write(&path, vec![0u8; n]).unwrap();
            let f = std::fs::File::options().write(true).open(&path).unwrap();
            f.set_modified(SystemTime::now() - Duration::from_secs(age_s)).unwrap();
        };
        write("extracts/old.osm.pbf", 400, 300);
        write("extracts/mid.osm.pbf", 400, 200);
        write("areas/active/tiles.mbtiles", 400, 500);
        let active = root.join("areas/active");
        std::fs::File::open(&active).unwrap().set_modified(SystemTime::now() - Duration::from_secs(1000)).ok();
        // 1200 bytes used; making room for 300 under 1300 requires freeing 200: only "old" goes.
        let freed = make_room(root, 1300, 300, std::slice::from_ref(&active));
        assert_eq!(freed, 400);
        assert!(!root.join("extracts/old.osm.pbf").exists());
        assert!(root.join("extracts/mid.osm.pbf").exists());
        assert!(active.join("tiles.mbtiles").exists());
    }
}

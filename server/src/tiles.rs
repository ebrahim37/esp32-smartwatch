//! Map tile proxy: fetches 256 px raster tiles from a legally cacheable, OSM-derived
//! upstream (the self-hosted tile server by default), re-colours them to a dark watch
//! palette, quantises to 16 colours and encodes them as SWT tiles.
//!
//! SWT ("smartwatch tile") is built for the wire and for the ESP32: a 58-byte header and a
//! zlib stream of the raw 4-bit pixels (two per byte, left pixel in the high nibble, 128
//! bytes per row). The watch inflates it with the ROM `tinfl` straight into a buffer and maps
//! indices through the palette into RGB565, so there is no PNG parsing, filtering or colour
//! conversion on the device. Sizes are on par with 4-bit PNG (a few KB for typical tiles).
//!
//! ```text
//! "SWT1" | width u16le | height u16le | format u8 (1 = 4-bit indexed) | colours u8 (16)
//!        | palette: colours x RGB888 | zlib(pixels)
//! ```
//!
//! Results are cached on disk under DATA_DIR/tiles/<area>/z/x/y.swt with size-bounded
//! eviction (the area namespace keeps tiles from an older map build from being served).

use std::{
    collections::VecDeque,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use image::{GenericImageView, Rgba};
use tokio::sync::Semaphore;

use crate::{
    config::TileRecolor,
    error::{AppError, AppResult},
};

/// Watch dark palette (16 entries).
pub const PALETTE: [[u8; 3]; 16] = [
    [0x12, 0x14, 0x17], // 0 land background
    [0x1C, 0x1F, 0x24], // 1 land light
    [0x26, 0x29, 0x2F], // 2 buildings
    [0x33, 0x37, 0x3E], // 3 minor roads edge
    [0x4A, 0x4F, 0x58], // 4 minor roads
    [0x6B, 0x70, 0x7A], // 5 roads
    [0x8E, 0x93, 0x9B], // 6 bright roads
    [0x7A, 0x5C, 0x2E], // 7 major road (amber)
    [0x9C, 0x74, 0x34], // 8 motorway
    [0x14, 0x2A, 0x44], // 9 water
    [0x1D, 0x3A, 0x5C], // 10 water light
    [0x17, 0x2E, 0x20], // 11 park
    [0x22, 0x40, 0x2C], // 12 forest light
    [0x3B, 0x2A, 0x2F], // 13 landuse warm
    [0xB8, 0xBD, 0xC6], // 14 labels
    [0xE6, 0xE8, 0xEB], // 15 bright labels
];

fn luma(r: u8, g: u8, b: u8) -> f32 {
    0.299 * r as f32 + 0.587 * g as f32 + 0.114 * b as f32
}

/// Classifies a light-style (OSM Carto-like) pixel into a dark palette index.
pub fn dark_index(px: Rgba<u8>) -> u8 {
    let [r, g, b, a] = px.0;
    if a < 32 {
        return 0;
    }
    let (rf, gf, bf) = (r as f32, g as f32, b as f32);
    let l = luma(r, g, b);
    let max = rf.max(gf).max(bf);
    let min = rf.min(gf).min(bf);
    let sat = if max > 0.0 { (max - min) / max } else { 0.0 };
    if l < 110.0 && sat < 0.35 {
        return if l < 60.0 { 15 } else { 14 }; // dark text/lines become light labels
    }
    if bf > rf + 18.0 && bf >= gf {
        return if l > 190.0 { 10 } else { 9 }; // water
    }
    if gf > rf + 10.0 && gf > bf + 5.0 {
        return if l > 200.0 { 12 } else { 11 }; // vegetation
    }
    if rf > 200.0 && gf > 120.0 && bf < 170.0 && sat > 0.25 {
        return if gf < 170.0 { 8 } else { 7 }; // orange/yellow major roads
    }
    if rf > gf + 15.0 && sat > 0.12 {
        return 13; // warm landuse (residential pink, retail)
    }
    if sat < 0.12 {
        return match l as u32 {
            250..=255 => 6, // white roads
            244..=249 => 4,
            236..=243 => 1, // land background (#f2efe9)
            215..=235 => 2, // buildings (#d9d0c9)
            170..=214 => 3,
            _ => 5,
        };
    }
    0
}

fn nearest_index(px: Rgba<u8>) -> u8 {
    let [r, g, b, _] = px.0;
    let mut best = (u32::MAX, 0u8);
    for (i, c) in PALETTE.iter().enumerate() {
        let d = (r as i32 - c[0] as i32).pow(2) as u32 + (g as i32 - c[1] as i32).pow(2) as u32 + (b as i32 - c[2] as i32).pow(2) as u32;
        if d < best.0 {
            best = (d, i as u8);
        }
    }
    best.1
}

pub const SWT_MAGIC: &[u8; 4] = b"SWT1";
pub const SWT_HEADER_LEN: usize = 10 + 16 * 3;

/// Encodes packed 4-bit pixels (256x256) as an SWT tile.
pub fn encode_swt(packed: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(SWT_HEADER_LEN + packed.len() / 4);
    out.extend_from_slice(SWT_MAGIC);
    out.extend_from_slice(&256u16.to_le_bytes());
    out.extend_from_slice(&256u16.to_le_bytes());
    out.push(1);
    out.push(16);
    out.extend(PALETTE.iter().flatten());
    out.extend(miniz_oxide::deflate::compress_to_vec_zlib(packed, 9));
    out
}

/// Decodes any supported raster, maps every pixel to the palette and encodes an SWT tile.
/// `quadrant` (0..4, row-major) selects a quarter of a 2x-density parent tile.
pub fn convert(src: &[u8], mode: TileRecolor, quadrant: Option<u8>) -> anyhow::Result<Vec<u8>> {
    let mut img = image::load_from_memory(src)?;
    let (w, h) = img.dimensions();
    anyhow::ensure!(w <= 1024 && h <= 1024, "tile too large");
    if let Some(q) = quadrant {
        let (hw, hh) = (w / 2, h / 2);
        img = img.crop_imm(u32::from(q & 1) * hw, u32::from(q >> 1) * hh, hw, hh);
    }
    let (w, h) = img.dimensions();
    let img = if w != 256 || h != 256 { img.resize_exact(256, 256, image::imageops::FilterType::Triangle) } else { img };
    let rgba = img.to_rgba8();
    let mut packed = Vec::with_capacity(256 * 128);
    for y in 0..256 {
        for x in (0..256).step_by(2) {
            let f = |x| match mode {
                TileRecolor::DarkHeuristic => dark_index(*rgba.get_pixel(x, y)),
                TileRecolor::None => nearest_index(*rgba.get_pixel(x, y)),
            };
            packed.push((f(x) << 4) | f(x + 1));
        }
    }
    Ok(encode_swt(&packed))
}

#[derive(Clone)]
pub struct TileService {
    http: reqwest::Client,
    template: String,
    mode: TileRecolor,
    scale: u8,
    dir: PathBuf,
    max_bytes: u64,
    upstream_limit: Arc<Semaphore>,
    /// Recently fetched 2x parent renders: one serves four neighbouring watch tiles.
    parents: Arc<Mutex<VecDeque<(TileKey, bytes::Bytes)>>>,
}

type TileKey = (u8, u32, u32);

/// `{r}` -> `@2x` (or inserted before the file extension) for 2x-density renders.
pub fn upstream_url(template: &str, z: u8, x: u32, y: u32, scale: u8) -> String {
    let r = if scale == 2 { "@2x" } else { "" };
    let mut t = template.to_string();
    if t.contains("{r}") {
        t = t.replace("{r}", r);
    } else if scale == 2 {
        let tail = t.rfind('/').unwrap_or(0);
        match t[tail..].rfind('.') {
            Some(dot) => t.insert_str(tail + dot, r),
            None => t.push_str(r),
        }
    }
    t.replace("{z}", &z.to_string()).replace("{x}", &x.to_string()).replace("{y}", &y.to_string())
}

impl TileService {
    pub fn new(http: reqwest::Client, template: String, mode: TileRecolor, scale: u8, data_dir: &Path, max_bytes: u64) -> Self {
        Self {
            http,
            template,
            mode,
            scale,
            dir: data_dir.join("tiles"),
            max_bytes,
            upstream_limit: Arc::new(Semaphore::new(4)),
            parents: Default::default(),
        }
    }

    fn path(&self, ns: &str, z: u8, x: u32, y: u32) -> PathBuf {
        let ns = if self.scale == 2 { format!("{ns}@2x") } else { ns.to_string() };
        self.dir.join(ns).join(z.to_string()).join(x.to_string()).join(format!("{y}.swt"))
    }

    async fn fetch(&self, z: u8, x: u32, y: u32, scale: u8) -> AppResult<bytes::Bytes> {
        let key = (z, x, y);
        if scale == 2 {
            if let Some((_, b)) = self.parents.lock().unwrap().iter().find(|(k, _)| *k == key) {
                return Ok(b.clone());
            }
        }
        // Bounded concurrency: be polite to the upstream tile server.
        let _permit = self.upstream_limit.acquire().await.map_err(|e| AppError::Internal(e.into()))?;
        let resp = self.http.get(upstream_url(&self.template, z, x, y, scale)).send().await?.error_for_status()?;
        let src = resp.bytes().await?;
        if src.len() > 4 * 1024 * 1024 {
            return Err(AppError::Upstream("tile too large".into()));
        }
        if scale == 2 {
            let mut p = self.parents.lock().unwrap();
            p.push_front((key, src.clone()));
            p.truncate(64);
        }
        Ok(src)
    }

    /// `ns` namespaces the disk cache (the map area id when map data is built on demand).
    pub async fn get(&self, ns: &str, z: u8, x: u32, y: u32) -> AppResult<Vec<u8>> {
        if z > 19 || x >= (1u32 << z) || y >= (1u32 << z) {
            return Err(AppError::BadRequest("tile out of range".into()));
        }
        let p = self.path(ns, z, x, y);
        if let Ok(b) = tokio::fs::read(&p).await {
            return Ok(b);
        }
        if self.template.is_empty() {
            return Err(AppError::NotConfigured("TILE_URL_TEMPLATE"));
        }
        let (src, quadrant) = if self.scale == 2 && z > 0 {
            (self.fetch(z - 1, x / 2, y / 2, 2).await?, Some(((y & 1) << 1 | (x & 1)) as u8))
        } else {
            (self.fetch(z, x, y, 1).await?, None)
        };
        let mode = self.mode;
        let out = tokio::task::spawn_blocking(move || convert(&src, mode, quadrant))
            .await
            .map_err(|e| AppError::Internal(e.into()))?
            .map_err(|e| AppError::Upstream(format!("tile decode: {e}")))?;
        if let Some(parent) = p.parent() {
            tokio::fs::create_dir_all(parent).await.ok();
        }
        let tmp = p.with_extension("tmp");
        if tokio::fs::write(&tmp, &out).await.is_ok() {
            tokio::fs::rename(&tmp, &p).await.ok();
        }
        Ok(out)
    }

    /// Deletes least-recently-modified tiles until the cache is under 90 % of its budget.
    pub fn evict(&self) -> std::io::Result<u64> {
        fn walk(d: &Path, out: &mut Vec<(std::time::SystemTime, u64, PathBuf)>) {
            if let Ok(rd) = std::fs::read_dir(d) {
                for e in rd.flatten() {
                    let p = e.path();
                    if let Ok(m) = e.metadata() {
                        if m.is_dir() {
                            walk(&p, out);
                        } else {
                            out.push((m.modified().unwrap_or(std::time::UNIX_EPOCH), m.len(), p));
                        }
                    }
                }
            }
        }
        let mut files = Vec::new();
        walk(&self.dir, &mut files);
        let mut total: u64 = files.iter().map(|f| f.1).sum();
        if total <= self.max_bytes {
            return Ok(0);
        }
        files.sort_by_key(|f| f.0);
        let target = self.max_bytes / 10 * 9;
        let mut freed = 0;
        for (_, len, p) in files {
            if total <= target {
                break;
            }
            if std::fs::remove_file(&p).is_ok() {
                total -= len;
                freed += len;
            }
        }
        Ok(freed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn classifies_typical_carto_colours() {
        assert_eq!(dark_index(Rgba([0xaa, 0xd3, 0xdf, 255])), 10); // water
        assert_eq!(dark_index(Rgba([0xf2, 0xef, 0xe9, 255])), 1); // land
        assert_eq!(dark_index(Rgba([0xff, 0xff, 0xff, 255])), 6); // road
        assert_eq!(dark_index(Rgba([0xcd, 0xeb, 0xb0, 255])), 12); // park
        assert_eq!(dark_index(Rgba([0x20, 0x20, 0x20, 255])), 15); // label
    }

    #[test]
    fn converts_to_small_indexed_png() {
        let mut img = image::RgbaImage::new(256, 256);
        for (x, y, p) in img.enumerate_pixels_mut() {
            *p = if (x / 32 + y / 32) % 2 == 0 { Rgba([0xf2, 0xef, 0xe9, 255]) } else { Rgba([0xaa, 0xd3, 0xdf, 255]) };
        }
        let mut src = Vec::new();
        image::DynamicImage::ImageRgba8(img).write_to(&mut Cursor::new(&mut src), image::ImageFormat::Png).unwrap();
        let out = convert(&src, TileRecolor::DarkHeuristic, None).unwrap();
        assert_eq!(&out[..4], SWT_MAGIC);
        assert_eq!(&out[4..10], &[0, 1, 0, 1, 1, 16]);
        assert!(out.len() < src.len());
        let pixels = miniz_oxide::inflate::decompress_to_vec_zlib(&out[SWT_HEADER_LEN..]).unwrap();
        assert_eq!(pixels.len(), 256 * 128);
        // (0,0) is land (index 1); x = 32 starts a water square (index 10).
        assert_eq!(pixels[0] >> 4, 1);
        assert_eq!(pixels[16] >> 4, 10);
    }

    #[test]
    fn upstream_url_scales() {
        let t = "http://h/styles/basic-preview/{z}/{x}/{y}.png";
        assert_eq!(upstream_url(t, 14, 1, 2, 2), "http://h/styles/basic-preview/14/1/2@2x.png");
        assert_eq!(upstream_url(t, 14, 1, 2, 1), "http://h/styles/basic-preview/14/1/2.png");
        assert_eq!(upstream_url("https://t/{z}/{x}/{y}{r}.webp", 3, 4, 5, 2), "https://t/3/4/5@2x.webp");
    }

    #[test]
    fn crops_quadrant_of_2x_parent() {
        let mut img = image::RgbaImage::from_pixel(512, 512, Rgba([255, 255, 255, 255]));
        for y in 256..512 {
            for x in 256..512 {
                img.put_pixel(x, y, Rgba([170, 211, 223, 255])); // water, bottom-right quadrant only
            }
        }
        let mut png = Vec::new();
        img.write_to(&mut Cursor::new(&mut png), image::ImageFormat::Png).unwrap();
        let px = |q| {
            let out = convert(&png, TileRecolor::DarkHeuristic, Some(q)).unwrap();
            miniz_oxide::inflate::decompress_to_vec_zlib(&out[SWT_HEADER_LEN..]).unwrap()[0] >> 4
        };
        assert_ne!(px(3), px(0));
        assert_eq!(px(1), px(0));
    }

}

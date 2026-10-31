//! Open-Meteo weather, summarised for the watch and cached per ~1 km grid cell.
//!
//! Precision rule: "time until precipitation" is derived from the 15-minutely forecast for
//! the next two hours and from hourly data beyond that, and reported with the resolution of
//! the data it came from (`resolution_min`). No finer precision is invented.

use std::{collections::HashMap, sync::Arc, time::Instant};

use serde::{Deserialize, Serialize};
use tokio::sync::Mutex;

use crate::error::{AppError, AppResult};

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Precip {
    /// "none" | "rain" | "snow" | "mixed"
    pub kind: &'static str,
    /// Minutes until precipitation starts (0 = now); None when none is forecast.
    pub in_minutes: Option<i64>,
    /// Forecast window covered by a "none" statement.
    pub horizon_h: u32,
    pub resolution_min: u32,
}

#[derive(Debug, Clone, Serialize)]
pub struct Day {
    pub date: i64,
    pub code: u8,
    pub low_c: f64,
    pub high_c: f64,
    pub precip_prob: f64,
    pub precip_mm: f64,
    pub uv_max: f64,
}

#[derive(Debug, Clone, Serialize)]
pub struct Hour {
    pub time: i64,
    pub code: u8,
    pub temp_c: f64,
    pub precip_prob: f64,
}

#[derive(Debug, Clone, Serialize)]
pub struct Summary {
    pub updated_at: i64,
    pub code: u8,
    pub description: &'static str,
    pub temp_c: f64,
    pub low_c: f64,
    pub high_c: f64,
    pub wind_kmh: f64,
    pub uv: f64,
    pub precip: Precip,
    pub days: Vec<Day>,
    pub hours: Vec<Hour>,
    pub source: &'static str,
}

#[derive(Debug, Deserialize)]
struct Current {
    time: i64,
    temperature_2m: f64,
    weather_code: u8,
    wind_speed_10m: f64,
    #[serde(default)]
    uv_index: Option<f64>,
}

#[derive(Debug, Deserialize, Default)]
struct Series {
    #[serde(default)]
    time: Vec<i64>,
    #[serde(default)]
    temperature_2m: Vec<Option<f64>>,
    #[serde(default)]
    weather_code: Vec<Option<u8>>,
    #[serde(default)]
    precipitation_probability: Vec<Option<f64>>,
    #[serde(default)]
    precipitation: Vec<Option<f64>>,
    #[serde(default)]
    rain: Vec<Option<f64>>,
    #[serde(default)]
    showers: Vec<Option<f64>>,
    #[serde(default)]
    snowfall: Vec<Option<f64>>,
}

#[derive(Debug, Deserialize, Default)]
struct Daily {
    #[serde(default)]
    time: Vec<i64>,
    #[serde(default)]
    weather_code: Vec<Option<u8>>,
    #[serde(default)]
    temperature_2m_max: Vec<Option<f64>>,
    #[serde(default)]
    temperature_2m_min: Vec<Option<f64>>,
    #[serde(default)]
    precipitation_probability_max: Vec<Option<f64>>,
    #[serde(default)]
    precipitation_sum: Vec<Option<f64>>,
    #[serde(default)]
    uv_index_max: Vec<Option<f64>>,
}

#[derive(Debug, Deserialize)]
pub struct OpenMeteo {
    current: Current,
    #[serde(default)]
    hourly: Series,
    #[serde(default)]
    minutely_15: Series,
    #[serde(default)]
    daily: Daily,
}

pub fn wmo_description(code: u8) -> &'static str {
    match code {
        0 => "clear",
        1 => "mostly clear",
        2 => "partly cloudy",
        3 => "overcast",
        45 | 48 => "foggy",
        51 | 53 | 55 => "drizzle",
        56 | 57 => "freezing drizzle",
        61 | 63 => "raining",
        65 => "heavy rain",
        66 | 67 => "freezing rain",
        71 | 73 => "snowing",
        75 => "heavy snow",
        77 => "snow grains",
        80 | 81 => "showers",
        82 => "heavy showers",
        85 | 86 => "snow showers",
        95 => "thunderstorm",
        96 | 99 => "hail storm",
        _ => "unknown",
    }
}

const WET_MM: f64 = 0.1;

fn at<T: Copy>(v: &[Option<T>], i: usize) -> Option<T> {
    v.get(i).copied().flatten()
}

fn kind_at(s: &Series, i: usize) -> &'static str {
    let snow = at(&s.snowfall, i).unwrap_or(0.0) > 0.0;
    let rain = at(&s.rain, i).unwrap_or(0.0) + at(&s.showers, i).unwrap_or(0.0) > 0.0;
    match (rain, snow) {
        (true, true) => "mixed",
        (false, true) => "snow",
        _ => "rain",
    }
}

/// First wet interval at or after `now`; `step` is the series' interval length in seconds.
fn first_wet(s: &Series, now: i64, step: i64) -> Option<(i64, &'static str)> {
    for (i, &t) in s.time.iter().enumerate() {
        if t + step <= now {
            continue; // interval entirely in the past
        }
        if at(&s.precipitation, i).unwrap_or(0.0) >= WET_MM {
            return Some(((t - now).max(0) / 60, kind_at(s, i)));
        }
    }
    None
}

fn round_to(v: i64, step: i64) -> i64 {
    (v + step / 2) / step * step
}

pub fn summarize(raw: &OpenMeteo, now: i64) -> Summary {
    let c = &raw.current;
    let precip = if let Some((m, kind)) = first_wet(&raw.minutely_15, now, 900) {
        Precip { kind, in_minutes: Some(round_to(m, 15)), horizon_h: 2, resolution_min: 15 }
    } else if let Some((m, kind)) = first_wet(&raw.hourly, now, 3600) {
        Precip { kind, in_minutes: Some(if m < 60 { m.max(0) } else { round_to(m, 60) }), horizon_h: 24, resolution_min: 60 }
    } else if raw.hourly.time.is_empty() {
        Precip { kind: "unknown", in_minutes: None, horizon_h: 0, resolution_min: 60 }
    } else {
        Precip { kind: "none", in_minutes: None, horizon_h: 24, resolution_min: 60 }
    };

    let d = &raw.daily;
    let days: Vec<Day> = (0..d.time.len().min(7))
        .map(|i| Day {
            date: d.time[i],
            code: at(&d.weather_code, i).unwrap_or(0),
            low_c: at(&d.temperature_2m_min, i).unwrap_or(f64::NAN),
            high_c: at(&d.temperature_2m_max, i).unwrap_or(f64::NAN),
            precip_prob: at(&d.precipitation_probability_max, i).unwrap_or(0.0),
            precip_mm: at(&d.precipitation_sum, i).unwrap_or(0.0),
            uv_max: at(&d.uv_index_max, i).unwrap_or(0.0),
        })
        .collect();

    let h = &raw.hourly;
    let hours: Vec<Hour> = h
        .time
        .iter()
        .enumerate()
        .filter(|(_, &t)| t + 3600 > now)
        .take(12)
        .map(|(i, &t)| Hour {
            time: t,
            code: at(&h.weather_code, i).unwrap_or(0),
            temp_c: at(&h.temperature_2m, i).unwrap_or(f64::NAN),
            precip_prob: at(&h.precipitation_probability, i).unwrap_or(0.0),
        })
        .collect();

    let (low, high) = days.first().map(|d| (d.low_c, d.high_c)).unwrap_or((c.temperature_2m, c.temperature_2m));
    Summary {
        updated_at: c.time,
        code: c.weather_code,
        description: wmo_description(c.weather_code),
        temp_c: c.temperature_2m,
        low_c: low.min(c.temperature_2m),
        high_c: high.max(c.temperature_2m),
        wind_kmh: c.wind_speed_10m,
        uv: c.uv_index.unwrap_or(0.0),
        precip,
        days,
        hours,
        source: "Open-Meteo",
    }
}

type Cache = Arc<Mutex<HashMap<(i32, i32), (Instant, Summary)>>>;

#[derive(Clone)]
pub struct WeatherService {
    http: reqwest::Client,
    base: String,
    ttl: std::time::Duration,
    cache: Cache,
}

impl WeatherService {
    pub fn new(http: reqwest::Client, base: String, ttl: std::time::Duration) -> Self {
        Self { http, base, ttl, cache: Arc::default() }
    }

    pub async fn get(&self, lat: f64, lon: f64) -> AppResult<Summary> {
        if !(-90.0..=90.0).contains(&lat) || !(-180.0..=180.0).contains(&lon) {
            return Err(AppError::BadRequest("lat/lon out of range".into()));
        }
        if self.base.is_empty() {
            return Err(AppError::NotConfigured("OPEN_METEO_URL"));
        }
        // ~1 km cells: nearby requests share one upstream call.
        let key = ((lat * 100.0).round() as i32, (lon * 100.0).round() as i32);
        if let Some((at, s)) = self.cache.lock().await.get(&key) {
            if at.elapsed() < self.ttl {
                return Ok(s.clone());
            }
        }
        let (qlat, qlon) = (key.0 as f64 / 100.0, key.1 as f64 / 100.0);
        let url = format!(
            "{}?latitude={qlat}&longitude={qlon}&current=temperature_2m,weather_code,wind_speed_10m,uv_index\
             &hourly=temperature_2m,weather_code,precipitation_probability,precipitation,rain,showers,snowfall\
             &minutely_15=precipitation,rain,showers,snowfall&forecast_minutely_15=8&forecast_hours=24\
             &daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,precipitation_sum,uv_index_max\
             &forecast_days=7&timezone=auto&timeformat=unixtime&wind_speed_unit=kmh",
            self.base
        );
        let raw: OpenMeteo = self.http.get(url).send().await?.error_for_status()?.json().await?;
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs() as i64;
        let s = summarize(&raw, now);
        self.cache.lock().await.insert(key, (Instant::now(), s.clone()));
        Ok(s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(minutely_wet: Option<usize>, hourly_wet: Option<usize>, snow: bool) -> OpenMeteo {
        let now = 1_761_840_000i64;
        let mut m = Series { time: (0..8).map(|i| now + i * 900).collect(), ..Default::default() };
        m.precipitation = vec![Some(0.0); 8];
        m.rain = vec![Some(0.0); 8];
        m.snowfall = vec![Some(0.0); 8];
        if let Some(i) = minutely_wet {
            m.precipitation[i] = Some(0.4);
            m.rain[i] = Some(0.4);
        }
        let mut h = Series { time: (0..24).map(|i| now + i * 3600).collect(), ..Default::default() };
        h.precipitation = vec![Some(0.0); 24];
        h.rain = vec![Some(0.0); 24];
        h.snowfall = vec![Some(0.0); 24];
        h.temperature_2m = vec![Some(5.0); 24];
        if let Some(i) = hourly_wet {
            h.precipitation[i] = Some(1.2);
            if snow {
                h.snowfall[i] = Some(0.8);
            } else {
                h.rain[i] = Some(1.2);
            }
        }
        OpenMeteo {
            current: Current { time: now, temperature_2m: 10.3, weather_code: 63, wind_speed_10m: 13.2, uv_index: Some(6.0) },
            hourly: h,
            minutely_15: m,
            daily: Daily {
                time: vec![now],
                weather_code: vec![Some(63)],
                temperature_2m_max: vec![Some(15.0)],
                temperature_2m_min: vec![Some(5.0)],
                precipitation_probability_max: vec![Some(80.0)],
                precipitation_sum: vec![Some(3.0)],
                uv_index_max: vec![Some(6.0)],
            },
        }
    }

    #[test]
    fn current_values_and_description() {
        let s = summarize(&fixture(None, None, false), 1_761_840_000);
        assert_eq!(s.description, "raining");
        assert_eq!((s.low_c, s.temp_c, s.high_c), (5.0, 10.3, 15.0));
        assert_eq!(s.precip.kind, "none");
        assert_eq!(s.precip.horizon_h, 24);
    }

    #[test]
    fn minutely_precipitation_uses_15_minute_resolution() {
        let s = summarize(&fixture(Some(2), Some(5), false), 1_761_840_000);
        assert_eq!(s.precip.in_minutes, Some(30));
        assert_eq!(s.precip.resolution_min, 15);
    }

    #[test]
    fn hourly_precipitation_rounds_to_hours_and_detects_snow() {
        let s = summarize(&fixture(None, Some(4), true), 1_761_840_000 + 600);
        assert_eq!(s.precip.kind, "snow");
        assert_eq!(s.precip.in_minutes, Some(240));
        assert_eq!(s.precip.resolution_min, 60);
    }

    #[test]
    fn precipitation_now() {
        let s = summarize(&fixture(Some(0), None, false), 1_761_840_000 + 300);
        assert_eq!(s.precip.in_minutes, Some(0));
    }
}

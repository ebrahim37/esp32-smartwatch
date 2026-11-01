//! POI / business search. Text queries go to Photon (OSM geocoder) with a location bias;
//! an empty query ("what's nearby") asks Overpass for named amenities and shops around the
//! user. Both are behind the `PoiSearch` trait so either engine (or Pelias) can be swapped.

use std::{future::Future, pin::Pin};

use serde::Serialize;
use serde_json::Value;

use crate::{
    error::{AppError, AppResult},
    geo::haversine_m,
};

pub type BoxFut<'a, T> = Pin<Box<dyn Future<Output = T> + Send + 'a>>;

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct Place {
    pub name: String,
    pub category: String,
    pub address: String,
    pub lat: f64,
    pub lon: f64,
    pub distance_m: u32,
}

pub trait PoiSearch: Send + Sync {
    fn search<'a>(&'a self, query: &'a str, lat: f64, lon: f64, limit: usize) -> BoxFut<'a, AppResult<Vec<Place>>>;
}

fn s(v: &Value, k: &str) -> String {
    v.get(k).and_then(|x| x.as_str()).unwrap_or_default().to_string()
}

fn pretty(tag: &str) -> String {
    let t = tag.replace('_', " ");
    let mut c = t.chars();
    c.next().map(|f| f.to_uppercase().collect::<String>() + c.as_str()).unwrap_or_default()
}

pub fn parse_photon(body: &Value, lat: f64, lon: f64) -> Vec<Place> {
    let mut out = Vec::new();
    for f in body.get("features").and_then(|f| f.as_array()).into_iter().flatten() {
        let coords = f.pointer("/geometry/coordinates").and_then(|c| c.as_array());
        let (Some(plon), Some(plat)) = (coords.and_then(|c| c.first()?.as_f64()), coords.and_then(|c| c.get(1)?.as_f64())) else {
            continue;
        };
        let p = &f["properties"];
        let name = s(p, "name");
        if name.is_empty() {
            continue;
        }
        let street = [s(p, "housenumber"), s(p, "street")].iter().filter(|x| !x.is_empty()).cloned().collect::<Vec<_>>().join(" ");
        let address = [street, s(p, "city")].iter().filter(|x| !x.is_empty()).cloned().collect::<Vec<_>>().join(", ");
        out.push(Place {
            name,
            category: pretty(&s(p, "osm_value")),
            address,
            lat: plat,
            lon: plon,
            distance_m: haversine_m(lat, lon, plat, plon).round() as u32,
        });
    }
    out
}

pub fn parse_overpass(body: &Value, lat: f64, lon: f64) -> Vec<Place> {
    let mut out: Vec<Place> = body
        .get("elements")
        .and_then(|e| e.as_array())
        .into_iter()
        .flatten()
        .filter_map(|e| {
            let tags = e.get("tags")?;
            let name = tags.get("name")?.as_str()?.to_string();
            let (plat, plon) = match (e.get("lat"), e.get("lon")) {
                (Some(a), Some(b)) => (a.as_f64()?, b.as_f64()?),
                _ => (e.pointer("/center/lat")?.as_f64()?, e.pointer("/center/lon")?.as_f64()?),
            };
            let kind =
                ["amenity", "shop", "tourism", "leisure"].iter().find_map(|k| tags.get(*k).and_then(|v| v.as_str())).unwrap_or("place");
            let street = [s(tags, "addr:housenumber"), s(tags, "addr:street")]
                .iter()
                .filter(|x| !x.is_empty())
                .cloned()
                .collect::<Vec<_>>()
                .join(" ");
            Some(Place {
                name,
                category: pretty(kind),
                address: street,
                lat: plat,
                lon: plon,
                distance_m: haversine_m(lat, lon, plat, plon).round() as u32,
            })
        })
        .collect();
    out.sort_by_key(|p| p.distance_m);
    out.dedup_by(|a, b| a.name == b.name && a.distance_m.abs_diff(b.distance_m) < 30);
    out
}

pub struct OsmSearch {
    pub http: reqwest::Client,
    pub photon_url: String,
    pub overpass_url: String,
}

impl PoiSearch for OsmSearch {
    fn search<'a>(&'a self, query: &'a str, lat: f64, lon: f64, limit: usize) -> BoxFut<'a, AppResult<Vec<Place>>> {
        Box::pin(async move {
            let limit = limit.clamp(1, 20);
            if query.trim().is_empty() {
                if self.overpass_url.is_empty() {
                    return Err(AppError::NotConfigured("OVERPASS_URL"));
                }
                let q = format!(
                    "[out:json][timeout:6];(nwr(around:600,{lat},{lon})[name][amenity];nwr(around:600,{lat},{lon})[name][shop];);out center {};",
                    limit * 3
                );
                let body: Value = self
                    .http
                    .post(&self.overpass_url)
                    .timeout(std::time::Duration::from_secs(8))
                    .form(&[("data", q)])
                    .send()
                    .await?
                    .error_for_status()?
                    .json()
                    .await?;
                let mut v = parse_overpass(&body, lat, lon);
                v.truncate(limit);
                Ok(v)
            } else {
                if self.photon_url.is_empty() {
                    return Err(AppError::NotConfigured("PHOTON_URL"));
                }
                let url = format!("{}/api", self.photon_url);
                let body: Value = self
                    .http
                    .get(url)
                    .query(&[("q", query), ("lat", &lat.to_string()), ("lon", &lon.to_string()), ("limit", &limit.to_string())])
                    .send()
                    .await?
                    .error_for_status()?
                    .json()
                    .await?;
                let mut v = parse_photon(&body, lat, lon);
                v.sort_by_key(|p| p.distance_m);
                Ok(v)
            }
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn photon_features_to_places() {
        let body = json!({"features":[
            {"geometry":{"coordinates":[-79.41,43.78]},"properties":{"name":"Cafe Uno","osm_value":"cafe","street":"Yonge St","housenumber":"5000","city":"Toronto"}},
            {"geometry":{"coordinates":[-79.40,43.70]},"properties":{"osm_value":"road"}}
        ]});
        let v = parse_photon(&body, 43.78, -79.41);
        assert_eq!(v.len(), 1);
        assert_eq!(v[0].category, "Cafe");
        assert_eq!(v[0].address, "5000 Yonge St, Toronto");
        assert!(v[0].distance_m < 5);
    }

    #[test]
    fn overpass_sorted_by_distance_with_way_centres() {
        let body = json!({"elements":[
            {"type":"node","lat":43.79,"lon":-79.41,"tags":{"name":"Far Shop","shop":"bakery"}},
            {"type":"way","center":{"lat":43.7801,"lon":-79.4119},"tags":{"name":"Near Pharmacy","amenity":"pharmacy"}},
            {"type":"node","lat":43.78,"lon":-79.41,"tags":{"amenity":"bench"}}
        ]});
        let v = parse_overpass(&body, 43.78008, -79.41186);
        assert_eq!(v.len(), 2);
        assert_eq!(v[0].name, "Near Pharmacy");
        assert_eq!(v[1].category, "Bakery");
    }
}

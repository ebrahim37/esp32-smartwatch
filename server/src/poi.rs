//! Local POI index for the active map area: named amenities, shops, tourism, transit and
//! places extracted from the area's OSM clip with osmium, searched in memory. Answers in
//! milliseconds and understands categories ("coffee" finds cafés, "gas" fuel stations); the
//! search handler falls back to Photon for anything outside the area or with few matches.

use std::path::Path;

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::{geo::haversine_m, places::Place};

/// OSM keys whose objects are worth finding, in the order used to name the category.
const KEYS: [&str; 13] = [
    "amenity",
    "shop",
    "tourism",
    "leisure",
    "office",
    "craft",
    "healthcare",
    "historic",
    "railway",
    "public_transport",
    "aeroway",
    "building",
    "place",
];

/// Everyday words that don't appear in OSM tag values.
const SYNONYMS: &[(&str, &[&str])] = &[
    ("coffee", &["cafe"]),
    ("gas", &["fuel"]),
    ("petrol", &["fuel"]),
    ("grocery", &["supermarket", "greengrocer", "convenience"]),
    ("groceries", &["supermarket", "greengrocer", "convenience"]),
    ("food", &["restaurant", "fast food", "food court"]),
    ("drugstore", &["pharmacy", "chemist"]),
    ("doctor", &["doctors", "clinic"]),
    ("toilet", &["toilets"]),
    ("washroom", &["toilets"]),
    ("restroom", &["toilets"]),
    ("bathroom", &["toilets"]),
    ("train", &["station", "halt"]),
    ("subway", &["station"]),
    ("gym", &["fitness centre"]),
    ("motel", &["motel", "hotel"]),
    ("library", &["library"]),
    ("charging", &["charging station"]),
];

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Poi {
    pub name: String,
    pub category: String,
    pub address: String,
    pub lat: f64,
    pub lon: f64,
    /// Lowercased name, and tag values (category, cuisine, brand) for matching.
    #[serde(skip)]
    name_lc: String,
    #[serde(default)]
    keys: String,
}

#[derive(Default)]
pub struct PoiIndex {
    pois: Vec<Poi>,
}

fn pretty(v: &str) -> String {
    let t = v.replace('_', " ");
    let mut c = t.chars();
    c.next().map(|f| f.to_uppercase().collect::<String>() + c.as_str()).unwrap_or_default()
}

/// A representative point: the point itself, or the mean of a line's / outer ring's vertices.
fn representative(geom: &Value) -> Option<(f64, f64)> {
    let coords = geom.get("coordinates")?;
    let ring = match geom.get("type")?.as_str()? {
        "Point" => return Some((coords.get(1)?.as_f64()?, coords.get(0)?.as_f64()?)),
        "LineString" => coords.as_array()?,
        "Polygon" => coords.get(0)?.as_array()?,
        "MultiPolygon" => coords.get(0)?.get(0)?.as_array()?,
        _ => return None,
    };
    let (mut lat, mut lon, mut n) = (0.0, 0.0, 0.0);
    for p in ring {
        lon += p.get(0)?.as_f64()?;
        lat += p.get(1)?.as_f64()?;
        n += 1.0;
    }
    (n > 0.0).then(|| (lat / n, lon / n))
}

/// Parses one GeoJSON feature (osmium export) into a POI; None for unnamed or unwanted ones.
pub fn parse_feature(f: &Value) -> Option<Poi> {
    let props = f.get("properties")?;
    let tag = |k: &str| props.get(k).and_then(|v| v.as_str()).unwrap_or("");
    let name = tag("name");
    if name.is_empty() {
        return None;
    }
    let (key, value) = KEYS.iter().find_map(|k| Some((*k, props.get(*k)?.as_str()?)))?;
    // Buildings only count when they are something people look for.
    if key == "building" && !matches!(value, "train_station" | "hospital" | "stadium" | "university" | "school" | "church") {
        return None;
    }
    let (lat, lon) = representative(f.get("geometry")?)?;
    let category = if value == "yes" { pretty(key) } else { pretty(value) };
    let street = [tag("addr:housenumber"), tag("addr:street")].iter().filter(|s| !s.is_empty()).copied().collect::<Vec<_>>().join(" ");
    let address = [street.as_str(), tag("addr:city")].iter().filter(|s| !s.is_empty()).copied().collect::<Vec<_>>().join(", ");
    let keys = [value, tag("cuisine"), tag("brand"), tag("sport")].join(" ").replace(['_', ';'], " ").to_lowercase();
    Some(Poi { name: name.to_string(), name_lc: name.to_lowercase(), category, address, lat, lon, keys })
}

impl PoiIndex {
    pub fn len(&self) -> usize {
        self.pois.len()
    }

    pub fn is_empty(&self) -> bool {
        self.pois.is_empty()
    }

    pub fn from_pois(mut pois: Vec<Poi>) -> Self {
        for p in &mut pois {
            p.name_lc = p.name.to_lowercase();
        }
        Self { pois }
    }

    /// Reads osmium's GeoJSON text sequence (RS-separated features).
    pub fn from_geojsonseq(text: &str) -> Self {
        let pois = text
            .split(['\x1e', '\n'])
            .filter(|l| !l.trim().is_empty())
            .filter_map(|l| serde_json::from_str::<Value>(l).ok())
            .filter_map(|f| parse_feature(&f))
            .collect();
        Self { pois }
    }

    pub fn load(path: &Path) -> Result<Self> {
        let b = std::fs::read(path).with_context(|| format!("read {}", path.display()))?;
        Ok(Self::from_pois(serde_json::from_slice(&b)?))
    }

    pub fn save(&self, path: &Path) -> Result<()> {
        let tmp = path.with_extension("tmp");
        std::fs::write(&tmp, serde_json::to_vec(&self.pois)?)?;
        std::fs::rename(&tmp, path)?;
        Ok(())
    }

    /// Empty query: named places within 1 km, nearest first. Otherwise every query word must
    /// match the name or a tag value (or a synonym); exact and prefix name matches rank as if
    /// they were closer.
    pub fn search(&self, query: &str, lat: f64, lon: f64, limit: usize) -> Vec<Place> {
        let q = query.trim().to_lowercase();
        let words: Vec<&str> = q.split_whitespace().collect();
        // "gas", "coffee", ...: the user means the category, not names starting with the word.
        let category_words = !words.is_empty() && words.iter().all(|w| SYNONYMS.iter().any(|(s, _)| s == w));
        let mut hits: Vec<(f64, &Poi, f64)> = Vec::new();
        for p in &self.pois {
            let in_category = || {
                words
                    .iter()
                    .all(|w| p.keys.contains(w) || SYNONYMS.iter().any(|(s, alts)| s == w && alts.iter().any(|a| p.keys.contains(a))))
            };
            let factor = if words.is_empty() {
                1.0
            } else if p.name_lc == q {
                10.0
            } else if category_words && in_category() {
                4.0
            } else if category_words {
                continue;
            } else if p.name_lc.starts_with(&q) {
                5.0
            } else if words.iter().all(|w| p.name_lc.split(|c: char| !c.is_alphanumeric()).any(|t| t.starts_with(w))) {
                3.0
            } else if words.iter().all(|w| {
                p.name_lc.contains(w)
                    || p.keys.contains(w)
                    || SYNONYMS.iter().any(|(s, alts)| s == w && alts.iter().any(|a| p.keys.contains(a)))
            }) {
                1.0
            } else {
                continue;
            };
            let d = haversine_m(lat, lon, p.lat, p.lon);
            if words.is_empty() && d > 1000.0 {
                continue;
            }
            hits.push((d / factor, p, d));
        }
        hits.sort_by(|a, b| a.0.total_cmp(&b.0));
        // The same place is often mapped twice (a point and its building outline).
        let mut picked: Vec<(&Poi, f64)> = Vec::new();
        for (_, p, d) in hits {
            if picked.len() >= limit {
                break;
            }
            if !picked.iter().any(|(q, _)| q.name == p.name && haversine_m(q.lat, q.lon, p.lat, p.lon) < 150.0) {
                picked.push((p, d));
            }
        }
        picked
            .into_iter()
            .map(|(p, d)| Place {
                name: p.name.clone(),
                category: p.category.clone(),
                address: p.address.clone(),
                lat: p.lat,
                lon: p.lon,
                distance_m: d.round() as u32,
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn feature(name: &str, tags: Value, lat: f64, lon: f64) -> String {
        let mut props = tags;
        props["name"] = json!(name);
        format!("\x1e{}\n", json!({"type": "Feature", "geometry": {"type": "Point", "coordinates": [lon, lat]}, "properties": props}))
    }

    fn index() -> PoiIndex {
        let mut s = String::new();
        s += &feature(
            "Starbucks",
            json!({"amenity": "cafe", "brand": "Starbucks", "addr:street": "Yonge Street", "addr:housenumber": "5000"}),
            43.781,
            -79.411,
        );
        s += &feature("Coffee Time", json!({"amenity": "cafe"}), 43.83, -79.41);
        s += &feature("Petro-Canada", json!({"amenity": "fuel"}), 43.79, -79.42);
        s += &feature("Yorkdale Shopping Centre", json!({"shop": "mall"}), 43.725, -79.452);
        s += &feature("Pizza Nova", json!({"amenity": "restaurant", "cuisine": "pizza"}), 43.782, -79.412);
        s += &format!(
            "\x1e{}\n",
            json!({"type": "Feature", "geometry": {"type": "Point", "coordinates": [-79.41, 43.78]}, "properties": {"amenity": "bench"}})
        );
        PoiIndex::from_geojsonseq(&s)
    }

    #[test]
    fn parses_named_features_only() {
        let idx = index();
        assert_eq!(idx.len(), 5);
        let r = idx.search("starbucks", 43.78, -79.41, 5);
        assert_eq!(r[0].address, "5000 Yonge Street");
        assert_eq!(r[0].category, "Cafe");
    }

    #[test]
    fn categories_and_synonyms() {
        let idx = index();
        let coffee: Vec<_> = idx.search("coffee", 43.78, -79.41, 5).into_iter().map(|p| p.name).collect();
        assert_eq!(coffee, ["Starbucks", "Coffee Time"]); // nearby cafe before a farther name match
        assert_eq!(idx.search("gas", 43.78, -79.41, 5)[0].name, "Petro-Canada");
        assert_eq!(idx.search("pizza", 43.78, -79.41, 5)[0].name, "Pizza Nova");
        assert_eq!(idx.search("yorkdale", 43.78, -79.41, 5)[0].name, "Yorkdale Shopping Centre");
        assert!(idx.search("sushi", 43.78, -79.41, 5).is_empty());
    }

    #[test]
    fn nearby_is_within_a_kilometre() {
        let names: Vec<_> = index().search("", 43.78, -79.41, 10).into_iter().map(|p| p.name).collect();
        assert_eq!(names, ["Starbucks", "Pizza Nova"]);
    }

    #[test]
    fn polygon_uses_ring_centre() {
        let f = json!({"type": "Feature", "properties": {"name": "Park", "leisure": "park"},
            "geometry": {"type": "Polygon", "coordinates": [[[0.0, 0.0], [2.0, 0.0], [2.0, 2.0], [0.0, 2.0]]]}});
        let p = parse_feature(&f).unwrap();
        assert_eq!((p.lat, p.lon), (1.0, 1.0));
    }

    #[test]
    fn save_and_load_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("pois.json");
        index().save(&path).unwrap();
        let back = PoiIndex::load(&path).unwrap();
        assert_eq!(back.len(), 5);
        assert_eq!(back.search("gas", 43.78, -79.41, 1)[0].name, "Petro-Canada");
    }
}

//! Directions via OSRM (self-hosted recommended). The route is simplified server-side to
//! at most 200 points and turned into short, watch-sized instructions.

use serde::Serialize;
use serde_json::Value;

use crate::{
    error::{AppError, AppResult},
    geo::{haversine_m, simplify_to},
    places::BoxFut,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Walk,
    Bike,
    Drive,
}

impl Mode {
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "walk" | "foot" => Some(Mode::Walk),
            "bike" | "bicycle" | "cycle" => Some(Mode::Bike),
            "drive" | "car" | "driving" => Some(Mode::Drive),
            _ => None,
        }
    }
    fn index(self) -> usize {
        match self {
            Mode::Walk => 0,
            Mode::Bike => 1,
            Mode::Drive => 2,
        }
    }
    fn osrm_profile(self) -> &'static str {
        match self {
            Mode::Walk => "foot",
            Mode::Bike => "bicycle",
            Mode::Drive => "driving",
        }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct Step {
    pub instruction: String,
    pub distance_m: u32,
    pub point_index: usize,
}

#[derive(Debug, Clone, Serialize)]
pub struct Route {
    pub status: &'static str,
    pub distance_m: u32,
    pub duration_s: u32,
    /// [lat, lon] pairs
    pub points: Vec<[f64; 2]>,
    pub steps: Vec<Step>,
}

pub trait Router: Send + Sync {
    fn route<'a>(&'a self, from: (f64, f64), to: (f64, f64), mode: Mode) -> BoxFut<'a, AppResult<Route>>;
}

fn instruction(m: &Value, name: &str) -> String {
    let kind = m.get("type").and_then(|v| v.as_str()).unwrap_or("");
    let modifier = m.get("modifier").and_then(|v| v.as_str()).unwrap_or("");
    let onto = if name.is_empty() { String::new() } else { format!(" onto {name}") };
    match kind {
        "depart" => {
            if name.is_empty() {
                "Start".into()
            } else {
                format!("Head along {name}")
            }
        }
        "arrive" => "Arrive at destination".into(),
        "roundabout" | "rotary" => format!("Take the roundabout{onto}"),
        "merge" => format!("Merge{onto}"),
        "fork" => format!("Keep {modifier}{onto}"),
        "end of road" | "turn" | "new name" | "continue" | "on ramp" | "off ramp" => match modifier {
            "straight" => format!("Continue{onto}"),
            "uturn" => "Make a U-turn".into(),
            "" => format!("Continue{onto}"),
            m => format!("Turn {m}{onto}"),
        },
        _ => format!("Continue{onto}"),
    }
}

pub fn parse_osrm(body: &Value) -> AppResult<Route> {
    if body.get("code").and_then(|c| c.as_str()) != Some("Ok") {
        return Ok(Route { status: "not_found", distance_m: 0, duration_s: 0, points: vec![], steps: vec![] });
    }
    let r = body.pointer("/routes/0").ok_or_else(|| AppError::Upstream("no route".into()))?;
    let coords: Vec<(f64, f64)> = r
        .pointer("/geometry/coordinates")
        .and_then(|c| c.as_array())
        .into_iter()
        .flatten()
        .filter_map(|p| Some((p.get(1)?.as_f64()?, p.get(0)?.as_f64()?)))
        .collect();
    let simplified = simplify_to(&coords, 200);
    let nearest = |lat: f64, lon: f64| {
        simplified
            .iter()
            .enumerate()
            .min_by(|a, b| haversine_m(lat, lon, a.1 .0, a.1 .1).total_cmp(&haversine_m(lat, lon, b.1 .0, b.1 .1)))
            .map(|(i, _)| i)
            .unwrap_or(0)
    };
    let mut steps = Vec::new();
    for leg in r.get("legs").and_then(|l| l.as_array()).into_iter().flatten() {
        for st in leg.get("steps").and_then(|s| s.as_array()).into_iter().flatten() {
            let m = &st["maneuver"];
            let name = st.get("name").and_then(|n| n.as_str()).unwrap_or("");
            let loc = m.get("location").and_then(|l| l.as_array());
            let (lon, lat) = (loc.and_then(|l| l.first()?.as_f64()).unwrap_or(0.0), loc.and_then(|l| l.get(1)?.as_f64()).unwrap_or(0.0));
            steps.push(Step {
                instruction: instruction(m, name),
                distance_m: st.get("distance").and_then(|d| d.as_f64()).unwrap_or(0.0).round() as u32,
                point_index: nearest(lat, lon),
            });
        }
    }
    steps.truncate(24);
    Ok(Route {
        status: "ok",
        distance_m: r.get("distance").and_then(|d| d.as_f64()).unwrap_or(0.0).round() as u32,
        duration_s: r.get("duration").and_then(|d| d.as_f64()).unwrap_or(0.0).round() as u32,
        points: simplified.iter().map(|p| [p.0, p.1]).collect(),
        steps,
    })
}

pub struct Osrm {
    pub http: reqwest::Client,
    /// Base URL; `{profile}` is substituted if present, otherwise `/route/v1/<profile>` is appended.
    pub base: String,
    /// Per-profile base URLs (foot, bicycle, driving) that take precedence over `base`.
    pub per_profile: [String; 3],
}

impl Osrm {
    fn prefix(&self, mode: Mode) -> Option<String> {
        let own = &self.per_profile[mode.index()];
        let base = if own.is_empty() { &self.base } else { own };
        if base.is_empty() {
            None
        } else if base.contains("{profile}") {
            Some(base.replace("{profile}", mode.osrm_profile()))
        } else {
            Some(format!("{}/route/v1/{}", base, mode.osrm_profile()))
        }
    }
}

impl Router for Osrm {
    fn route<'a>(&'a self, from: (f64, f64), to: (f64, f64), mode: Mode) -> BoxFut<'a, AppResult<Route>> {
        Box::pin(async move {
            let Some(prefix) = self.prefix(mode) else { return Err(AppError::NotConfigured("OSRM_URL")) };
            let url = format!("{prefix}/{},{};{},{}?overview=full&geometries=geojson&steps=true", from.1, from.0, to.1, to.0);
            let body: Value = self.http.get(url).send().await?.json().await?;
            parse_osrm(&body)
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn parses_osrm_route_with_steps() {
        let body = json!({"code":"Ok","routes":[{"distance":1234.5,"duration":900.2,
        "geometry":{"coordinates":[[-79.41,43.78],[-79.40,43.78],[-79.40,43.79]]},
        "legs":[{"steps":[
            {"name":"Yonge Street","distance":800.0,"maneuver":{"type":"depart","location":[-79.41,43.78]}},
            {"name":"Finch Avenue","distance":434.5,"maneuver":{"type":"turn","modifier":"left","location":[-79.40,43.78]}},
            {"name":"","distance":0.0,"maneuver":{"type":"arrive","location":[-79.40,43.79]}}
        ]}]}]});
        let r = parse_osrm(&body).unwrap();
        assert_eq!(r.status, "ok");
        assert_eq!(r.distance_m, 1235);
        assert_eq!(r.points.len(), 3);
        assert_eq!(r.points[0], [43.78, -79.41]);
        assert_eq!(r.steps[0].instruction, "Head along Yonge Street");
        assert_eq!(r.steps[1].instruction, "Turn left onto Finch Avenue");
        assert_eq!(r.steps[1].point_index, 1);
        assert_eq!(r.steps[2].instruction, "Arrive at destination");
    }

    #[test]
    fn no_route() {
        let r = parse_osrm(&json!({"code":"NoRoute"})).unwrap();
        assert_eq!(r.status, "not_found");
    }

    #[test]
    fn per_profile_urls_override_base() {
        let o = Osrm {
            http: reqwest::Client::new(),
            base: "https://o/{profile}".into(),
            per_profile: ["http://127.0.0.1:5001".into(), String::new(), String::new()],
        };
        assert_eq!(o.prefix(Mode::Walk).unwrap(), "http://127.0.0.1:5001/route/v1/foot");
        assert_eq!(o.prefix(Mode::Drive).unwrap(), "https://o/driving");
        let none = Osrm { http: reqwest::Client::new(), base: String::new(), per_profile: Default::default() };
        assert!(none.prefix(Mode::Bike).is_none());
    }
}

//! End-to-end API tests against an in-process mock upstream (no internet needed).

use std::net::SocketAddr;

use axum::{
    body::Body,
    http::{Request, StatusCode},
    routing::get,
    Json, Router,
};
use http_body_util::BodyExt;
use serde_json::{json, Value};
use sw_helper::{app, config::Config};
use tower::ServiceExt;

const TOKEN: &str = "test-token-0123456789abcdef";

async fn mock_upstream() -> SocketAddr {
    let png = {
        let img = image::RgbaImage::from_pixel(256, 256, image::Rgba([0xf2, 0xef, 0xe9, 255]));
        let mut v = Vec::new();
        image::DynamicImage::ImageRgba8(img).write_to(&mut std::io::Cursor::new(&mut v), image::ImageFormat::Png).unwrap();
        v
    };
    let r = Router::new()
        .route(
            "/forecast",
            get(|| async {
                Json(json!({
                    "current": {"time": 1761840000, "temperature_2m": 10.0, "weather_code": 63, "wind_speed_10m": 13.0, "uv_index": 6.0},
                    "hourly": {"time": [1761840000, 1761843600], "temperature_2m": [10.0, 9.0], "weather_code": [63, 61],
                               "precipitation_probability": [80, 60], "precipitation": [0.0, 0.0], "rain": [0.0, 0.0], "showers": [0.0,0.0], "snowfall": [0.0, 0.0]},
                    "daily": {"time": [1761800000], "weather_code": [63], "temperature_2m_max": [15.0], "temperature_2m_min": [5.0],
                              "precipitation_probability_max": [80], "precipitation_sum": [3.0], "uv_index_max": [6.0]}
                }))
            }),
        )
        .route("/tiles/{z}/{x}/{y}", get(move || async move { png.clone() }))
        .route(
            "/route/v1/foot/{coords}",
            get(|| async {
                Json(json!({"code": "Ok", "routes": [{"distance": 500.0, "duration": 400.0,
                    "geometry": {"coordinates": [[-79.41, 43.78], [-79.40, 43.78]]},
                    "legs": [{"steps": [{"name": "Main St", "distance": 500.0, "maneuver": {"type": "depart", "location": [-79.41, 43.78]}}]}]}]}))
            }),
        )
        .route(
            "/api",
            get(|| async { Json(json!({"features": [{"geometry": {"coordinates": [-79.41, 43.781]}, "properties": {"name": "Cafe", "osm_value": "cafe"}}]})) }),
        );
    let l = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = l.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(l, r).await.unwrap() });
    addr
}

async fn app_with_upstream() -> (Router, tempfile::TempDir) {
    let up = mock_upstream().await;
    let dir = tempfile::tempdir().unwrap();
    let mut cfg = Config::for_tests(dir.path().to_path_buf());
    cfg.open_meteo_url = format!("http://127.0.0.1:{}/forecast", up.port());
    cfg.tile_url_template = format!("http://127.0.0.1:{}/tiles/{{z}}/{{x}}/{{y}}", up.port());
    cfg.osrm_url = format!("http://127.0.0.1:{}", up.port());
    cfg.photon_url = format!("http://127.0.0.1:{}", up.port());
    let st = app::AppState::new(cfg).unwrap();
    (app::router(st), dir)
}

async fn call(app: &Router, method: &str, uri: &str, auth: bool, body: Option<Value>) -> (StatusCode, Vec<u8>) {
    let mut b = Request::builder().method(method).uri(uri);
    if auth {
        b = b.header("authorization", format!("Bearer {TOKEN}"));
    }
    let req = match body {
        Some(v) => b.header("content-type", "application/json").body(Body::from(v.to_string())).unwrap(),
        None => b.body(Body::empty()).unwrap(),
    };
    let resp = app.clone().oneshot(req).await.unwrap();
    let status = resp.status();
    (status, resp.into_body().collect().await.unwrap().to_bytes().to_vec())
}

fn json_of(b: &[u8]) -> Value {
    serde_json::from_slice(b).unwrap()
}

#[tokio::test]
async fn health_is_public_but_api_requires_token() {
    let (app, _d) = app_with_upstream().await;
    assert_eq!(call(&app, "GET", "/healthz", false, None).await.0, StatusCode::OK);
    assert_eq!(call(&app, "GET", "/v1/status", false, None).await.0, StatusCode::UNAUTHORIZED);
    let (s, _) = call(&app, "GET", "/v1/status", true, None).await;
    assert_eq!(s, StatusCode::OK);
}

#[tokio::test]
async fn weather_summary() {
    let (app, _d) = app_with_upstream().await;
    let (s, b) = call(&app, "GET", "/v1/weather?lat=43.78&lon=-79.41", true, None).await;
    assert_eq!(s, StatusCode::OK);
    let v = json_of(&b);
    assert_eq!(v["description"], "raining");
    assert_eq!(v["low_c"], 5.0);
    assert_eq!(v["precip"]["kind"], "none");
    let (s, _) = call(&app, "GET", "/v1/weather?lat=123&lon=0", true, None).await;
    assert_eq!(s, StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn tile_is_converted_and_cached() {
    let (app, dir) = app_with_upstream().await;
    let (s, b) = call(&app, "GET", "/v1/maps/tile/15/9155/11961.swt", true, None).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(&b[..4], b"SWT1");
    assert!(dir.path().join("tiles/default/15/9155/11961.swt").exists());
    let (s, _) = call(&app, "GET", "/v1/maps/tile/3/99/0.swt", true, None).await;
    assert_eq!(s, StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn route_and_search() {
    let (app, _d) = app_with_upstream().await;
    let (s, b) = call(&app, "GET", "/v1/maps/route?from=43.78,-79.41&to=43.78,-79.40&mode=walk", true, None).await;
    assert_eq!(s, StatusCode::OK);
    let v = json_of(&b);
    assert_eq!(v["status"], "ok");
    assert_eq!(v["steps"][0]["instruction"], "Head along Main St");
    let (s, b) = call(&app, "GET", "/v1/maps/search?q=cafe&lat=43.78&lon=-79.41", true, None).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(json_of(&b)["places"][0]["name"], "Cafe");
}

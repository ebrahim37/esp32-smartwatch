//! Geometry helpers: distances and Douglas-Peucker polyline simplification.

/// Centre of slippy-map tile z/x/y as (lat, lon).
pub fn tile_center(z: u8, x: u32, y: u32) -> (f64, f64) {
    let n = (1u64 << z) as f64;
    let lon = (x as f64 + 0.5) / n * 360.0 - 180.0;
    let lat = (std::f64::consts::PI * (1.0 - 2.0 * (y as f64 + 0.5) / n)).sinh().atan().to_degrees();
    (lat, lon)
}

/// Tile containing (lat, lon) at zoom z.
pub fn tile_at(lat: f64, lon: f64, z: u8) -> (u32, u32) {
    let n = (1u64 << z) as f64;
    let x = ((lon + 180.0) / 360.0 * n).floor().clamp(0.0, n - 1.0);
    let lr = lat.clamp(-85.05, 85.05).to_radians();
    let y = ((1.0 - (lr.tan() + 1.0 / lr.cos()).ln() / std::f64::consts::PI) / 2.0 * n).floor().clamp(0.0, n - 1.0);
    (x as u32, y as u32)
}

pub fn haversine_m(lat1: f64, lon1: f64, lat2: f64, lon2: f64) -> f64 {
    let r = 6_371_000.0;
    let (p1, p2) = (lat1.to_radians(), lat2.to_radians());
    let dp = p2 - p1;
    let dl = (lon2 - lon1).to_radians();
    let a = (dp / 2.0).sin().powi(2) + p1.cos() * p2.cos() * (dl / 2.0).sin().powi(2);
    2.0 * r * a.sqrt().asin()
}

/// Perpendicular distance (m) of `p` from segment a-b, using a local equirectangular
/// projection (accurate enough at route scales).
fn seg_dist_m(p: (f64, f64), a: (f64, f64), b: (f64, f64)) -> f64 {
    let k = p.0.to_radians().cos();
    let to_xy = |q: (f64, f64)| (q.1 * k * 111_320.0, q.0 * 110_540.0);
    let (px, py) = to_xy(p);
    let (ax, ay) = to_xy(a);
    let (bx, by) = to_xy(b);
    let (dx, dy) = (bx - ax, by - ay);
    let len2 = dx * dx + dy * dy;
    let t = if len2 > 0.0 { (((px - ax) * dx + (py - ay) * dy) / len2).clamp(0.0, 1.0) } else { 0.0 };
    ((px - ax - t * dx).powi(2) + (py - ay - t * dy).powi(2)).sqrt()
}

/// Douglas-Peucker with tolerance in metres. Points are (lat, lon).
pub fn simplify(points: &[(f64, f64)], tolerance_m: f64) -> Vec<(f64, f64)> {
    if points.len() <= 2 {
        return points.to_vec();
    }
    let mut keep = vec![false; points.len()];
    keep[0] = true;
    keep[points.len() - 1] = true;
    let mut stack = vec![(0usize, points.len() - 1)];
    while let Some((s, e)) = stack.pop() {
        let mut best = (0.0, 0usize);
        for i in s + 1..e {
            let d = seg_dist_m(points[i], points[s], points[e]);
            if d > best.0 {
                best = (d, i);
            }
        }
        if best.0 > tolerance_m {
            keep[best.1] = true;
            stack.push((s, best.1));
            stack.push((best.1, e));
        }
    }
    points.iter().zip(keep).filter(|(_, k)| *k).map(|(p, _)| *p).collect()
}

/// Simplifies until at most `max_points` remain, doubling the tolerance as needed.
pub fn simplify_to(points: &[(f64, f64)], max_points: usize) -> Vec<(f64, f64)> {
    let mut tol = 3.0;
    let mut out = simplify(points, tol);
    while out.len() > max_points && tol < 5000.0 {
        tol *= 2.0;
        out = simplify(points, tol);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn haversine_one_hundredth_degree() {
        assert!((haversine_m(43.0, -79.0, 43.01, -79.0) - 1111.95).abs() < 1.0);
    }

    #[test]
    fn simplify_drops_collinear_points() {
        let pts: Vec<(f64, f64)> = (0..100).map(|i| (43.0 + i as f64 * 1e-4, -79.0)).collect();
        assert_eq!(simplify(&pts, 1.0).len(), 2);
    }

    #[test]
    fn simplify_keeps_corners_and_respects_limit() {
        let mut pts: Vec<(f64, f64)> = (0..50).map(|i| (43.0 + i as f64 * 1e-4, -79.0)).collect();
        pts.extend((1..50).map(|i| (43.0049, -79.0 + i as f64 * 1e-4)));
        let s = simplify(&pts, 1.0);
        assert_eq!(s.len(), 3);
        let zig: Vec<(f64, f64)> = (0..1000).map(|i| (43.0 + i as f64 * 1e-4, -79.0 + if i % 2 == 0 { 0.0 } else { 3e-4 })).collect();
        assert!(simplify_to(&zig, 200).len() <= 200);
    }
}

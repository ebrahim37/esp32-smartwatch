use anyhow::Context;
use sw_helper::{app, config::Config};
use tracing_subscriber::EnvFilter;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt().with_env_filter(EnvFilter::try_from_default_env().unwrap_or_else(|_| "info,tower_http=info".into())).init();
    let cfg = Config::from_env()?;
    std::fs::create_dir_all(&cfg.data_dir).context("DATA_DIR")?;
    let state = app::AppState::new(cfg.clone())?;

    // Housekeeping: bound the tile cache.
    let hk_tiles = state.tiles.clone();
    tokio::spawn(async move {
        let mut t = tokio::time::interval(std::time::Duration::from_secs(3600));
        loop {
            t.tick().await;
            let tiles = hk_tiles.clone();
            if let Ok(Ok(freed)) = tokio::task::spawn_blocking(move || tiles.evict()).await {
                if freed > 0 {
                    tracing::info!("evicted {} MB of cached tiles", freed / 1_048_576);
                }
            }
        }
    });

    let listener = tokio::net::TcpListener::bind(cfg.bind).await?;
    tracing::info!("listening on {}", cfg.bind);
    axum::serve(listener, app::router(state))
        .with_graceful_shutdown(async {
            // SIGTERM is what Podman/systemd send on stop; SIGINT for interactive use.
            let mut term = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()).expect("SIGTERM handler");
            tokio::select! {
                _ = tokio::signal::ctrl_c() => {}
                _ = term.recv() => {}
            }
            tracing::info!("shutting down");
        })
        .await?;
    Ok(())
}

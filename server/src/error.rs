use axum::{
    http::StatusCode,
    response::{IntoResponse, Response},
    Json,
};
use serde_json::json;

#[derive(Debug, thiserror::Error)]
pub enum AppError {
    #[error("bad request: {0}")]
    BadRequest(String),
    #[error("not configured: {0}")]
    NotConfigured(&'static str),
    #[error("upstream error: {0}")]
    Upstream(String),
    #[error("not found")]
    NotFound,
    #[error("map data for this area is being prepared; retry in a minute")]
    Preparing,
    #[error("internal error: {0}")]
    Internal(#[from] anyhow::Error),
}

impl From<reqwest::Error> for AppError {
    fn from(e: reqwest::Error) -> Self {
        AppError::Upstream(e.without_url().to_string())
    }
}

impl IntoResponse for AppError {
    fn into_response(self) -> Response {
        let status = match &self {
            AppError::BadRequest(_) => StatusCode::BAD_REQUEST,
            AppError::NotConfigured(_) => StatusCode::SERVICE_UNAVAILABLE,
            AppError::Upstream(_) => StatusCode::BAD_GATEWAY,
            AppError::NotFound => StatusCode::NOT_FOUND,
            AppError::Preparing => StatusCode::SERVICE_UNAVAILABLE,
            AppError::Internal(_) => StatusCode::INTERNAL_SERVER_ERROR,
        };
        if matches!(self, AppError::Internal(_) | AppError::Upstream(_)) {
            tracing::warn!("{self}");
        }
        let retry = matches!(self, AppError::Preparing);
        let mut resp = (status, Json(json!({ "error": self.to_string(), "preparing": retry }))).into_response();
        if retry {
            resp.headers_mut().insert(axum::http::header::RETRY_AFTER, axum::http::HeaderValue::from_static("60"));
        }
        resp
    }
}

pub type AppResult<T> = Result<T, AppError>;

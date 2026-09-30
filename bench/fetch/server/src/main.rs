// The server the fetch() clients call: axum on tokio's multi-thread runtime (WORKERS threads,
// default 4), so the server is never what limits a client.
use axum::{routing::{get, post}, Json, Router};
use serde::Serialize;
use std::sync::OnceLock;

#[derive(Serialize)]
struct User {
    id: i64,
    name: &'static str,
    email: &'static str,
}

static BIG: OnceLock<Vec<u8>> = OnceLock::new();

fn main() {
    let port: u16 = std::env::var("PORT").ok().and_then(|p| p.parse().ok()).unwrap_or(3000);
    let workers: usize = std::env::var("WORKERS").ok().and_then(|w| w.parse().ok()).unwrap_or(4);
    // /big: 8 MiB of text
    BIG.set((0..8 << 20).map(|i| b'a' + (i % 26) as u8).collect()).unwrap();
    let app = Router::new()
        .route("/", get(|| async { "Hello, World!" }))
        .route("/json", get(|| async { Json(User { id: 1, name: "Ada Lovelace", email: "ada@example.com" }) }))
        .route("/echo", post(|body: String| async { body }))
        .route("/big", get(|| async { BIG.get().unwrap().as_slice() }));
    let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(workers).enable_all().build().unwrap();
    rt.block_on(async move {
        let listener = tokio::net::TcpListener::bind(("127.0.0.1", port)).await.unwrap();
        axum::serve(listener, app).await.unwrap();
    });
}

// axum on tokio — same routes as server.barm. WORKERS=1 is a current-thread runtime,
// WORKERS>1 a multi-thread runtime with that many workers (or thread-per-core with MODE=tpc).
use axum::{http::StatusCode, response::IntoResponse, routing::{get, post}, Json, Router};
use serde::Serialize;

#[derive(Serialize)]
struct User {
    id: i64,
    name: &'static str,
    email: &'static str,
}

async fn hello() -> &'static str {
    "Hello, World!"
}

async fn json() -> impl IntoResponse {
    Json(User { id: 1, name: "Ada Lovelace", email: "ada@example.com" })
}

async fn echo(body: String) -> String {
    body
}

async fn not_found() -> impl IntoResponse {
    (StatusCode::NOT_FOUND, "not found")
}

fn app() -> Router {
    Router::new().route("/", get(hello)).route("/json", get(json)).route("/echo", post(echo)).fallback(not_found)
}

/// One current-thread runtime serving its own SO_REUSEPORT listener (thread-per-core).
fn serve_current_thread(port: u16, reuse: bool) {
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
    rt.block_on(async move {
        let sock = tokio::net::TcpSocket::new_v4().unwrap();
        sock.set_reuseaddr(true).unwrap();
        if reuse {
            sock.set_reuseport(true).unwrap();
        }
        sock.bind(([0, 0, 0, 0], port).into()).unwrap();
        let listener = sock.listen(4096).unwrap();
        axum::serve(listener, app()).await.unwrap();
    });
}

fn main() {
    let port: u16 = std::env::var("PORT").ok().and_then(|p| p.parse().ok()).unwrap_or(3000);
    let workers: usize = std::env::var("WORKERS").ok().and_then(|w| w.parse().ok()).unwrap_or(1);
    // MODE=tpc: thread-per-core (one current-thread runtime per worker, kernel-balanced
    // SO_REUSEPORT listeners). Default: tokio's work-stealing multi-thread runtime.
    let tpc = std::env::var("MODE").is_ok_and(|m| m == "tpc");
    if workers <= 1 {
        return serve_current_thread(port, false);
    }
    if tpc {
        let threads: Vec<_> = (0..workers).map(|_| std::thread::spawn(move || serve_current_thread(port, true))).collect();
        for t in threads {
            t.join().unwrap();
        }
        return;
    }
    let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(workers).enable_all().build().unwrap();
    rt.block_on(async move {
        let listener = tokio::net::TcpListener::bind(("0.0.0.0", port)).await.unwrap();
        axum::serve(listener, app()).await.unwrap();
    });
}

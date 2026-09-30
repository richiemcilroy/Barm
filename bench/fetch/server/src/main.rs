// The server the fetch() clients call: axum on tokio's multi-thread runtime (WORKERS threads,
// default 4), so the server is never what limits a client.
use axum::{routing::{get, post}, Json, Router};
use serde::Serialize;
use std::sync::{Arc, OnceLock};

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
    // TLS_PORT: the same routes over HTTPS, with tests/fetch/tls's certificate (for localhost,
    // signed by its test CA)
    let tls_port: Option<u16> = std::env::var("TLS_PORT").ok().and_then(|p| p.parse().ok());
    rt.block_on(async move {
        if let Some(tls_port) = tls_port {
            let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/fetch/tls");
            let certs = pem_items(&std::fs::read(dir.join("server.pem")).unwrap(), "CERTIFICATE").into_iter().map(tokio_rustls::rustls::pki_types::CertificateDer::from).collect();
            let key = pem_items(&std::fs::read(dir.join("server.key")).unwrap(), "PRIVATE KEY").remove(0);
            let key = tokio_rustls::rustls::pki_types::PrivateKeyDer::Pkcs8(key.into());
            let cfg = tokio_rustls::rustls::ServerConfig::builder().with_no_client_auth().with_single_cert(certs, key).unwrap();
            let acceptor = tokio_rustls::TlsAcceptor::from(Arc::new(cfg));
            let listener = tokio::net::TcpListener::bind(("127.0.0.1", tls_port)).await.unwrap();
            let app = app.clone();
            tokio::spawn(async move {
                loop {
                    let Ok((tcp, _)) = listener.accept().await else { continue };
                    let _ = tcp.set_nodelay(true);
                    let (acceptor, app) = (acceptor.clone(), app.clone());
                    tokio::spawn(async move {
                        let Ok(tls) = acceptor.accept(tcp).await else { return };
                        let service = hyper_util::service::TowerToHyperService::new(app);
                        let _ = hyper::server::conn::http1::Builder::new().serve_connection(hyper_util::rt::TokioIo::new(tls), service).await;
                    });
                }
            });
        }
        let listener = tokio::net::TcpListener::bind(("127.0.0.1", port)).await.unwrap();
        axum::serve(listener, app).await.unwrap();
    });
}

/// The DER bytes of each `-----BEGIN <label>-----` block in a PEM file.
fn pem_items(pem: &[u8], label: &str) -> Vec<Vec<u8>> {
    let text = String::from_utf8_lossy(pem);
    let (begin, end) = (format!("-----BEGIN {label}-----"), format!("-----END {label}-----"));
    let mut out = Vec::new();
    let mut rest = text.as_ref();
    while let Some(b) = rest.find(&begin) {
        let after = &rest[b + begin.len()..];
        let e = after.find(&end).unwrap();
        out.push(base64(&after[..e]));
        rest = &after[e + end.len()..];
    }
    out
}

fn base64(s: &str) -> Vec<u8> {
    let val = |c: u8| match c {
        b'A'..=b'Z' => c - b'A',
        b'a'..=b'z' => c - b'a' + 26,
        b'0'..=b'9' => c - b'0' + 52,
        b'+' => 62,
        _ => 63,
    };
    let chars: Vec<u8> = s.bytes().filter(|c| c.is_ascii_alphanumeric() || *c == b'+' || *c == b'/').collect();
    let mut out = Vec::new();
    for chunk in chars.chunks(4) {
        let mut v = 0u32;
        for (i, &c) in chunk.iter().enumerate() {
            v |= (val(c) as u32) << (18 - 6 * i);
        }
        for i in 0..chunk.len().saturating_sub(1) {
            out.push((v >> (16 - 8 * i)) as u8);
        }
    }
    out
}

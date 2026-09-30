//! fetch(): each tests/fetch/<name>.barm runs against a scripted HTTP/1.1 server (below) that covers
//! the protocol's corners: chunked and close-delimited bodies, gzip/deflate, interim 1xx
//! responses, redirects of every kind, keep-alive reuse, servers that drop connections, slow
//! responses (aborts and timeouts), malformed responses. Its output must equal
//! tests/fetch/<name>.stdout, which is the same program's output under Bun; when `bun` is
//! installed the expectation is re-checked against it (except for native_* programs, which cover
//! what Barm does differently or Bun can't run).

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

struct Req {
    method: String,
    path: String,
    query: String,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

impl Req {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
    fn param(&self, name: &str) -> Option<String> {
        self.query.split('&').find_map(|kv| kv.strip_prefix(name).and_then(|r| r.strip_prefix('=')).map(|v| v.to_string()))
    }
}

fn read_req(s: &mut TcpStream, buf: &mut Vec<u8>) -> Option<Req> {
    loop {
        if let Some(end) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            let head = String::from_utf8_lossy(&buf[..end]).into_owned();
            let mut lines = head.split("\r\n");
            let first = lines.next()?;
            let mut parts = first.split(' ');
            let method = parts.next()?.to_string();
            let target = parts.next()?.to_string();
            let (path, query) = match target.split_once('?') {
                Some((p, q)) => (p.to_string(), q.to_string()),
                None => (target.clone(), String::new()),
            };
            let headers: Vec<(String, String)> = lines.filter_map(|l| l.split_once(':').map(|(k, v)| (k.trim().to_string(), v.trim().to_string()))).collect();
            let len: usize = headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("content-length")).and_then(|(_, v)| v.parse().ok()).unwrap_or(0);
            while buf.len() < end + 4 + len {
                let mut chunk = [0u8; 65536];
                let n = s.read(&mut chunk).ok()?;
                if n == 0 {
                    return None;
                }
                buf.extend_from_slice(&chunk[..n]);
            }
            let body = buf[end + 4..end + 4 + len].to_vec();
            buf.drain(..end + 4 + len);
            return Some(Req { method, path, query, headers, body });
        }
        let mut chunk = [0u8; 65536];
        let n = s.read(&mut chunk).ok()?;
        if n == 0 {
            return None;
        }
        buf.extend_from_slice(&chunk[..n]);
    }
}

fn respond(s: &mut TcpStream, status: &str, headers: &[(&str, String)], body: &[u8]) {
    let mut out = format!("HTTP/1.1 {status}\r\nContent-Length: {}\r\n", body.len()).into_bytes();
    for (k, v) in headers {
        out.extend_from_slice(format!("{k}: {v}\r\n").as_bytes());
    }
    out.extend_from_slice(b"\r\n");
    out.extend_from_slice(body);
    let _ = s.write_all(&out);
}

/// Serves one connection; returns false when it should be closed.
fn handle(s: &mut TcpStream, r: &Req, served: usize, fixtures: &Path, alt: &str) -> bool {
    let fixture = |name: &str| std::fs::read(fixtures.join(name)).unwrap();
    match r.path.as_str() {
        "/text" => respond(s, "200 OK", &[("Content-Type", "text/plain".into())], b"hello"),
        "/json" => respond(s, "200 OK", &[("Content-Type", "application/json".into())], br#"{"a":1,"b":[1,2],"c":"x"}"#),
        "/echo" => {
            let mut out = format!("{} {}", r.method, if r.query.is_empty() { String::new() } else { format!("?{}", r.query) });
            for h in ["content-type", "content-length", "authorization", "cookie", "x-custom", "x-multi"] {
                if let Some(v) = r.header(h) {
                    out.push_str(&format!("|{h}={v}"));
                }
            }
            out.push_str(&format!("|body={}", String::from_utf8_lossy(&r.body)));
            respond(s, "200 OK", &[], out.as_bytes());
        }
        "/chunked" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3;ext=1\r\nhel\r\n2\r\nlo\r\nA\r\n, chunked!\r\n0\r\nX-Trailer: 1\r\n\r\n");
        }
        "/chunked-big" => {
            let mut out = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n".to_vec();
            for i in 0..500 {
                let chunk = format!("{:0>1000}", i);
                out.extend_from_slice(format!("{:x}\r\n{chunk}\r\n", chunk.len()).as_bytes());
            }
            out.extend_from_slice(b"0\r\n\r\n");
            let _ = s.write_all(&out);
        }
        "/close" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nread until the server closes");
            return false;
        }
        "/http10" => {
            let _ = s.write_all(b"HTTP/1.0 200 OK\r\n\r\nan HTTP/1.0 body");
            return false;
        }
        "/lf" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\ncontent-length: 7\nx-lf: yes\n\nlf only");
        }
        "/continue" => {
            let _ = s.write_all(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n");
            respond(s, "200 OK", &[], b"after interim responses");
        }
        "/status" => {
            let code = r.param("code").unwrap_or_else(|| "200".into());
            let reason = r.param("reason").unwrap_or_default().replace("%20", " ");
            respond(s, &format!("{code} {reason}"), &[], format!("status {code}").as_bytes());
        }
        "/nobody" => {
            let code = r.param("code").unwrap();
            let _ = s.write_all(format!("HTTP/1.1 {code} X\r\nContent-Length: 0\r\n\r\n").as_bytes());
        }
        "/head" => respond(s, "200 OK", &[("X-Head", "1".into())], b"body not sent for HEAD"),
        "/redirect" => {
            let code = r.param("code").unwrap_or_else(|| "302".into());
            let to = r.param("to").unwrap_or_else(|| "/echo".into()).replace("%3A", ":").replace("%2F", "/").replace("ALT", alt);
            respond(s, &format!("{code} Redirect"), &[("Location", to)], b"redirecting");
        }
        "/chain" => {
            let n: u32 = r.param("n").and_then(|v| v.parse().ok()).unwrap_or(0);
            if n == 0 {
                respond(s, "200 OK", &[], b"end of chain");
            } else {
                respond(s, "302 Found", &[("Location", format!("/chain?n={}", n - 1))], b"");
            }
        }
        "/loop" => respond(s, "302 Found", &[("Location", "/loop".into())], b""),
        "/headers" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-A: 1\r\nx-a: 2\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nX-Spaces:   padded value  \r\n\r\nok");
        }
        "/gzip" | "/deflate" | "/br" | "/zstd" => {
            let name = r.param("f").unwrap();
            let enc = &r.path[1..];
            respond(s, "200 OK", &[("Content-Encoding", enc.into())], &fixture(&name));
        }
        "/badgzip" => respond(s, "200 OK", &[("Content-Encoding", "gzip".into())], b"\x1f\x8b\x08\x00not really gzip at all"),
        "/badbr" => respond(s, "200 OK", &[("Content-Encoding", "br".into())], b"not brotli at all, really not"),
        "/badzstd" => respond(s, "200 OK", &[("Content-Encoding", "zstd".into())], b"\x28\xb5\x2f\xfd not zstd after the magic"),
        "/utf8" => respond(s, "200 OK", &[], b"a\xffb\xe2\x82 c\xf0\x9f\x98\x80d"),
        "/bom" => respond(s, "200 OK", &[], b"\xef\xbb\xbfbom"),
        "/big" => {
            let n: usize = r.param("n").and_then(|v| v.parse().ok()).unwrap_or(1000);
            let body: Vec<u8> = (0..n).map(|i| b'a' + (i % 26) as u8).collect();
            respond(s, "200 OK", &[], &body);
        }
        "/slow" => {
            let ms: u64 = r.param("ms").and_then(|v| v.parse().ok()).unwrap_or(1000);
            std::thread::sleep(Duration::from_millis(ms));
            respond(s, "200 OK", &[], b"slow");
        }
        "/conn" => respond(s, "200 OK", &[], format!("request {} on this connection", served + 1).as_bytes()),
        "/closing" => {
            respond(s, "200 OK", &[("Connection", "close".into())], b"closing after this");
            return false;
        }
        "/drop" => return false,
        "/malformed" => {
            let _ = s.write_all(b"this is not HTTP\r\n\r\n");
            return false;
        }
        "/short" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly part");
            return false;
        }
        _ => respond(s, "404 Not Found", &[], b"no such route"),
    }
    true
}

/// The HTTPS test servers (python3 tests/fetch/tls/server.py): ports for the good, expired and
/// self-signed certificates.
struct TlsServer {
    child: std::process::Child,
    ports: [u16; 3],
}

impl TlsServer {
    fn start(script: &Path) -> Option<TlsServer> {
        let mut child = Command::new("python3").arg(script).stdout(std::process::Stdio::piped()).stderr(std::process::Stdio::null()).spawn().ok()?;
        let mut line = String::new();
        std::io::BufRead::read_line(&mut std::io::BufReader::new(child.stdout.take()?), &mut line).ok()?;
        let ports: Vec<u16> = line.split_whitespace().skip(1).filter_map(|p| p.parse().ok()).collect();
        let ports: [u16; 3] = ports.try_into().ok()?;
        Some(TlsServer { child, ports })
    }
}

impl Drop for TlsServer {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn serve(listener: TcpListener, fixtures: PathBuf, alt: String, stop: Arc<AtomicBool>) {
    for conn in listener.incoming() {
        if stop.load(Ordering::Relaxed) {
            break;
        }
        let Ok(mut s) = conn else { continue };
        let fixtures = fixtures.clone();
        let alt = alt.clone();
        std::thread::spawn(move || {
            let mut buf = Vec::new();
            let mut served = 0;
            while let Some(r) = read_req(&mut s, &mut buf) {
                if !handle(&mut s, &r, served, &fixtures, &alt) {
                    break;
                }
                served += 1;
            }
        });
    }
}

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let dir = root.join("tests/fetch");
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let base = format!("http://127.0.0.1:{port}");
    let alt = format!("http://localhost:{port}");
    let stop = Arc::new(AtomicBool::new(false));
    // BARM_FETCH_SERVE=<alt origin>: only serve (for running the clients elsewhere, e.g. under
    // sanitizers in a container); prints the port.
    if let Ok(alt) = std::env::var("BARM_FETCH_SERVE") {
        let alt = alt.replace("PORT", &port.to_string());
        println!("{port}");
        serve(listener, dir, alt, stop);
        return;
    }
    {
        let (dir, alt, stop) = (dir.clone(), alt.clone(), stop.clone());
        std::thread::spawn(move || serve(listener, dir, alt, stop));
    }
    // Every tests/fetch/<name>.barm; the ones not named native_* are also checked against Bun.
    let mut cases: Vec<PathBuf> = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "barm")).collect();
    cases.sort();
    let mut failed = Vec::new();
    let bun = Command::new("bun").arg("--version").output().is_ok_and(|o| o.status.success());
    // HTTPS: tests/fetch/tls/server.py (tls.barm needs it; skipped without python3)
    let tls = TlsServer::start(&dir.join("tls/server.py"));
    let mut skipped = 0;
    for case in &cases {
        let name = case.file_stem().unwrap().to_string_lossy().into_owned();
        let mut env: Vec<(&str, String)> = vec![("BASE", base.clone()), ("ALT", alt.clone())];
        if name == "tls" {
            let Some(t) = &tls else {
                skipped += 1;
                continue;
            };
            env.extend([("TLS_GOOD", t.ports[0].to_string()), ("TLS_EXPIRED", t.ports[1].to_string()), ("TLS_SELF", t.ports[2].to_string())]);
        }
        let expected = std::fs::read_to_string(case.with_extension("stdout")).unwrap_or_default();
        if bun && !name.starts_with("native_") {
            let out = Command::new("sh").arg(root.join("scripts/barm2js.sh")).arg(case).current_dir(&root).env("BARM2JS_RUNTIME", "bun").envs(env.iter().map(|(k, v)| (*k, v.as_str()))).output().expect("run bun");
            let js = String::from_utf8_lossy(&out.stdout);
            if js != expected {
                failed.push(format!("{name}: the expectation differs from Bun's output\n--- expected\n{expected}--- bun\n{js}{}", String::from_utf8_lossy(&out.stderr)));
            }
        }
        let opts = barm::build::Options { mode: barm::codegen::Mode::Run, unchecked: false, opt: "-O1".into(), emit_c: None };
        match barm::build::build(std::slice::from_ref(case), &root, &opts) {
            Ok(built) => {
                if !built.tls {
                    failed.push(format!("{name}: fetches, but doesn't link TLS"));
                }
                let started = std::time::Instant::now();
                let out = Command::new(&built.binary).current_dir(&root).envs(env.iter().map(|(k, v)| (*k, v.as_str()))).output().expect("run binary");
                let actual = String::from_utf8_lossy(&out.stdout).into_owned();
                // nothing here waits long: a slow exit means something kept the loop alive
                if started.elapsed() > Duration::from_secs(20) {
                    failed.push(format!("{name}: took {:?}", started.elapsed()));
                }
                if actual != expected || !out.status.success() {
                    failed.push(format!("{name}: output differs (exit {:?})\n--- expected\n{expected}--- actual\n{actual}{}", out.status.code(), String::from_utf8_lossy(&out.stderr)));
                }
            }
            Err(barm::build::BuildError::Diagnostics(sm, d)) => failed.push(format!("{name}: build failed\n{}", barm::diag::render_text(&d, &sm))),
            Err(barm::build::BuildError::Message(m)) => failed.push(format!("{name}: {m}")),
        }
    }
    // Only programs that fetch link TLS.
    let opts = barm::build::Options { mode: barm::codegen::Mode::Run, unchecked: false, opt: "-O1".into(), emit_c: None };
    let hello = std::env::temp_dir().join(format!("barm-no-tls-{}.barm", std::process::id()));
    std::fs::write(&hello, "console.log(\"hi\")\n").unwrap();
    match barm::build::build(std::slice::from_ref(&hello), &root, &opts) {
        Ok(b) if b.tls => failed.push("a program that doesn't fetch links TLS".into()),
        Ok(_) => {}
        Err(_) => failed.push("hello-world didn't build".into()),
    }
    let _ = std::fs::remove_file(&hello);
    stop.store(true, Ordering::Relaxed);
    let _ = TcpStream::connect(("127.0.0.1", port));
    for f in &failed {
        eprintln!("{f}\n");
    }
    drop(tls);
    let skip_note = if skipped > 0 { format!(", {skipped} skipped (no python3 for the HTTPS server)") } else { String::new() };
    println!("fetch: {} programs{}{skip_note}, {} failed", cases.len() - skipped, if bun { " (checked against Bun)" } else { "" }, failed.len());
    if !failed.is_empty() {
        std::process::exit(1);
    }
}

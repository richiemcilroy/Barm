# HTTP server benchmark

The same three routes served by Barm (`std/http`), Rust (axum on tokio), Bun (`Bun.serve`) and Node (`node:http`), driven by a keep-alive load generator:

| route | request | response |
|---|---|---|
| `hello` | `GET /` | `Hello, World!` (text/plain) |
| `json` | `GET /json` | `{"id":1,"name":"Ada Lovelace","email":"ada@example.com"}`, serialized per request |
| `echo` | `POST /echo` with a 75-byte JSON body | the body |

Files:
- `server.barm` is the Barm server.
- `rust/` is the Rust server (axum 0.8, tokio 1, LTO, `panic = "abort"`).
- `server.bun.ts` is the Bun server, and `server.node.mjs` is the Node server.
- `load.c` is the load generator.
- `run.py` is the runner.
- `linux.sh` runs the benchmark on Linux in Docker.

## Results

Apple M4 Max (12 performance + 4 efficiency cores). The load generator runs on the same machine as the server, with 8 threads and 128 persistent connections, one request in flight per connection. Servers are interleaved across 3 repetitions and the median is shown. `cpu/req` is the server's CPU time per request, and `rss` is its resident memory at the end of the run.

### Linux (Docker VM, 16 vCPUs, epoll)

One process or thread:

| route | Barm | Rust (axum) | Bun | Barm p99 | Rust p99 | Bun p99 |
|---|---:|---:|---:|---:|---:|---:|
| hello | **423k req/s** | 285k | 255k | 1.15 ms | 1.32 ms | 1.44 ms |
| json | **400k** | 277k | 234k | 1.18 ms | 1.61 ms | 1.50 ms |
| echo | **402k** | 254k | 198k | 1.22 ms | 1.23 ms | 1.61 ms |
| cpu/req (json) | **1.46 µs** | 2.50 µs | 3.01 µs | | | |
| rss | **3.3 MB** | 5.8 MB | 40 MB | | | |

Four workers. Barm and Bun use 4 processes. For Rust:
- `rust` is tokio's multi-thread runtime with 4 workers, the axum default.
- `rust-tpc` is thread-per-core: 4 current-thread runtimes with `SO_REUSEPORT` listeners, the layout the fastest Rust servers use.

| route | Barm | Rust tpc | Rust | Bun |
|---|---:|---:|---:|---:|
| hello | **1.71M req/s** | 1.28M | 671k | 966k |
| json | **1.63M** | 1.11M | 613k | 790k |
| echo | **1.66M** | 965k | 716k | 660k |
| p99 (json) | **0.19 ms** | 1.06 ms | 0.68 ms | 1.42 ms |
| cpu/req (json) | **2.45 µs** | 3.01 µs | 3.64 µs | 3.83 µs |
| rss | 9 MB | 6 MB | 8 MB | 178 MB |

Pipelined (16 requests in flight per connection, as in TechEmpower's plaintext test), json route:

| | Barm | Rust tpc | Rust | Bun |
|---|---:|---:|---:|---:|
| 1 worker | **3.1M req/s** | 618k | 569k | 48k |
| 4 workers | **13.3M** | 2.2M | 1.6M | 194k |

### macOS (kqueue)

Here every server runs into the loopback cap of about 180k round trips/s (see Caveats), so req/s mostly shows who reaches the cap. `cpu/req` (µs of server CPU per request) is the better comparison.

One process or thread:

| route | Barm | Rust (axum) | Bun | Node |
|---|---:|---:|---:|---:|
| hello | 171k · 5.8 µs | **185k · 5.4 µs** | 145k · 7.2 µs | 112k · 9.0 µs |
| json | 175k · 5.7 µs | 176k · 5.7 µs | 142k · 7.3 µs | 109k · 9.3 µs |
| echo | **174k · 5.7 µs** | 155k · 6.4 µs | 131k · 7.9 µs | 98k · 10.3 µs |
| rss | **4 MB** | 8 MB | 39 MB | 97 MB |

On one core, Barm and Rust sit at the same floor: over 95% of the time is the `read` and `write` syscalls. Which one comes out ahead changes from run to run.

Four workers:

| route | Barm | Rust (axum) | Bun | Node |
|---|---:|---:|---:|---:|
| hello | **170k · 8.1 µs** | 142k · 14.1 µs | 120k · 8.5 µs | 122k · 20.2 µs |
| json | **156k · 8.9 µs** | 155k · 13.2 µs | 117k · 8.8 µs | 124k · 19.6 µs |
| echo | 143k · **9.9 µs** | 133k · 16.9 µs | 104k · 9.8 µs | **134k** · 19.5 µs |

Bun's `cpu/req` stays low here because its extra processes receive no connections on macOS, so Bun is effectively one process.

Pipelined, one worker:

| route | Barm | Rust | Bun | Node |
|---|---:|---:|---:|---:|
| hello | **2.02M** | 503k | 18k | 138k |
| json | **1.47M** | 443k | 15k | 135k |
| echo | **1.57M** | 351k | 113k | 100k |

## How Barm gets there

- **Native core.** The event loop, HTTP/1.1 parser and response writer are C in the runtime (`runtime/barm.c`, `bm_native_http*`). The API is Barm (`crates/barm/src/std/http.barm`): `serve` passes a closure to the loop, which calls it for each request and writes the returned `Response`.
- **Level-triggered events, no wasted syscalls.** When a `read` fills less than the buffer, the socket is known to be drained, so the loop skips the extra `read` that would only return `EAGAIN`. A non-pipelined request costs one `read` and one `write`. With profiling on macOS, over 95% of server time is in those two syscalls.
- **Batched output.** Every response produced from one read goes out in one `write`, which is why pipelined throughput is 5–7× Rust's. hyper, under axum's defaults, flushes after each response (`pipeline_flush` is off).
- **Cheap requests.** A request is a few small strings from the runtime's small-object free lists, a `Request` object and a `Response` object, all reference-counted and freed as soon as the handler returns. No GC, and no per-request futures or tasks. The `date` header is formatted once per second.
- **Workers are processes.** `fork` after `listen` gives isolated heaps with no locking. A shared-memory table of per-worker connection counts makes each worker accept only while it is among the least loaded, so persistent connections spread evenly (macOS has no `SO_REUSEPORT` balancing).
- **Backpressure and limits.** A client that pipelines without reading stops being served once 1 MB of output is queued, and its socket stops being read until the output drains. Request limits:
  - headers: 64 KB (`431`)
  - bodies: 64 MB (`413`)
  - malformed framing, including `Content-Length` together with `Transfer-Encoding`: `400`, then the connection is closed

## Caveats

- **Loopback, same machine.** Load and server share the machine. On macOS, loopback round trips are capped system-wide at about 180k/s: three separate load processes together get the same total as one. Past that point every server looks alike in req/s, and `cpu/req` is the useful number.
- **Docker VM noise.** On an M-series host the VM's vCPUs sometimes land on efficiency cores. A worker process that lands there can't hand its connections to another worker, so single runs vary by up to 30%. The repetitions and interleaving exist for this reason.
- **Pipelining** is rare from browsers, but common between proxies and backends. The non-pipelined tables are the everyday case.
- **Rust.** axum is idiomatic Rust, not the fastest possible Rust. Hand-written servers on `may_minihttp` or raw `mio` would do better. `rust-tpc` is included so the comparison isn't only against tokio's work-stealing scheduler.
- **Bun.** `reusePort` only balances connections on Linux, so on macOS extra Bun processes sit idle. Bun's pipelined throughput collapses on this build (1.4.0).
- **Node** isn't in the Linux runs: the container has no Node, and it trails Bun on macOS.
- **Barm features.** Barm's server has no TLS, HTTP/2, streaming bodies or async handlers yet. Handlers run to completion one at a time per worker.

## Running

```sh
python3 bench/http/run.py                               # macOS/Linux host: barm, rust, bun, node
python3 bench/http/run.py --workers 1 --routes json     # a subset
python3 bench/http/run.py --pipeline 16 --repeat 1
bench/http/linux.sh --workers 1,4 --threads 8 --conns 128   # Linux in Docker (gcc:14, oven/bun:1.4.0)
out/load -c 64 -t 4 -d 5 http://127.0.0.1:3000/         # the load generator alone (-P pipeline, -m/-b for POST)
```

`linux.sh` does the following:
- Emits the Barm server as C and compiles it in the container.
- Cross-compiles the Rust server with `zig cc`. rustc's aarch64 `--fix-cortex-a53-843419` link flag is filtered out, because zig doesn't know it.
- Copies `bun` out of the `oven/bun` image.

Results are written to `bench/results/http[-linux]-<timestamp>.json`.

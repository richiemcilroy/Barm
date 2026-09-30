# HTTP server benchmark

The same three routes served by Barm, Rust (axum on tokio), Bun and Node (`node:http`), driven by a keep-alive load generator. The Barm server is the Bun server's own code — `Bun.serve`, `new URL(req.url).pathname`, `Response.json` — with a `try` added where a call can throw: it measures idiomatic code, not a hand-tuned path.

| route | request | response |
|---|---|---|
| `hello` | `GET /` | `Hello, World!` (text/plain) |
| `json` | `GET /json` | `{"id":1,"name":"Ada Lovelace","email":"ada@example.com"}`, serialized per request |
| `echo` | `POST /echo` with a 75-byte JSON body | the body |

Files:
- `server.barm.ts` is the Barm server (`diff server.bun.ts server.barm.ts` shows the edits).
- `rust/` is the Rust server (axum 0.8, tokio 1, LTO, `panic = "abort"`).
- `server.bun.ts` is the Bun server, and `server.node.mjs` is the Node server.
- `load.c` is the load generator.
- `run.py` is the runner.
- `linux.sh` runs the benchmark on Linux in Docker.

## Results

Apple M4 Max (12 performance + 4 efficiency cores). The load generator runs on the same machine as the server, with 8 threads and 128 persistent connections, one request in flight per connection. Servers are interleaved across 3 repetitions and the median is shown. `cpu/req` is the server's CPU time per request. `mem` is the memory the server costs the machine: the proportional set size (PSS) on Linux and the physical footprint on macOS, summed over its processes, so pages that forked workers share are counted once. `rss` (resident set, summed) is in the JSON results too; it counts shared pages once per process, which inflates multi-process servers.

### Linux (Docker VM, 16 vCPUs, epoll)

One process or thread:

| route | Barm | Rust (axum) | Bun |
|---|---:|---:|---:|
| hello | **408k req/s** | 314k | 277k |
| json | **386k** | 299k | 259k |
| echo | **388k** | 279k | 211k |
| cpu/req (json) | **1.57 µs** | 2.14 µs | 2.67 µs |
| mem (json) | **1.1 MB** | 4.9 MB | 39 MB |

Four workers. Barm and Bun use 4 processes. For Rust:
- `rust` is tokio's multi-thread runtime with 4 workers, the axum default.
- `rust-tpc` is thread-per-core: 4 current-thread runtimes with `SO_REUSEPORT` listeners, the layout the fastest Rust servers use.

| route | Barm | Rust tpc | Rust | Bun |
|---|---:|---:|---:|---:|
| hello | **1.69M req/s** | 1.38M | 727k | 1.07M |
| json | **1.65M** | 1.27M | 720k | 964k |
| echo | **1.67M** | 1.13M | 772k | 759k |
| cpu/req (json) | **2.35 µs** | 2.91 µs | 3.19 µs | 3.61 µs |
| mem (json) | **2.5 MB** | 5.0 MB | 6.3 MB | 98 MB |

Pipelined (16 requests in flight per connection, as in TechEmpower's plaintext test):

| | Barm | Rust tpc | Rust | Bun |
|---|---:|---:|---:|---:|
| hello, 1 worker | **3.87M req/s** | 773k | 798k | 52k |
| json, 1 worker | **2.37M** | 620k | 669k | 52k |
| hello, 4 workers | **10.4M** | 3.1M | 2.3M | 211k |
| json, 4 workers | **8.1M** | 2.4M | 2.2M | 203k |

#### Latency at a fixed load

The tables above are closed loop: each server runs as fast as it can, so tail latency there mixes in how hard the machine is being pushed. To compare latency, the load generator also runs open loop (`--rate`, like wrk2): requests go out on a fixed schedule every server can sustain, and each is timed from when it *should* have been sent.

| json route | Barm p50 | p99 | p99.9 | Rust tpc p50 | p99 | p99.9 | Rust p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 worker at 200k req/s | **334 µs** | **1.30 ms** | **1.67 ms** | 1.11 ms | 2.05 ms | 2.98 ms | 1.11 ms | 2.02 ms | 2.76 ms |
| 4 workers at 600k req/s | **170 µs** | **1.30 ms** | **1.58 ms** | 435 µs | 1.91 ms | 2.28 ms | 570 µs | 2.04 ms | 2.64 ms |

Bun can't hold these rates open loop (it falls to 38k and 158k): when requests queue on a connection they arrive pipelined, where Bun is slow (see the pipelined table).

At a fixed partial load Barm uses a little more CPU per request than Rust (2.70 vs 2.27 µs at 200k): it answers each request as soon as it arrives, so it reads more often. Rust reaches the socket later, finds several requests waiting and reads them at once; that wait is its higher p50. Total syscalls per request are the same (strace: 390k vs 399k over the same run).

### macOS (kqueue)

Here every server runs into the loopback cap of about 200k round trips/s (see Caveats), so req/s mostly shows who reaches the cap first. CPU per request is the better comparison. macOS has no PSS, and its per-process footprint counts pages that forked workers share once per process, so memory here is plain RSS summed over processes.

One process or thread:

| route | Barm | Rust (axum) | Bun | Node |
|---|---:|---:|---:|---:|
| hello | **202k · 4.9 µs** | 200k · 5.0 µs | 169k · 6.1 µs | 113k · 9.0 µs |
| json | **211k · 4.8 µs** | 195k · 5.2 µs | 164k · 6.3 µs | 111k · 9.1 µs |
| echo | **207k · 4.9 µs** | 176k · 5.7 µs | 150k · 6.8 µs | 97k · 10.5 µs |
| rss (json) | **2.7 MB** | 7.7 MB | 38 MB | 101 MB |

Four workers:

| route | Barm | Rust (axum) | Bun | Node |
|---|---:|---:|---:|---:|
| hello | **189k · 7.6 µs** | 171k · 11.9 µs | 171k · 6.1 µs | 169k · 15.4 µs |
| json | **189k · 7.9 µs** | 172k · 12.0 µs | 168k · 6.2 µs | 170k · 15.4 µs |
| echo | **189k · 8.0 µs** | 167k · 14.4 µs | 149k · 6.9 µs | 163k · 17.0 µs |
| rss (json) | 10.7 MB | **8.8 MB** | 94 MB | 394 MB |

Two rows Barm doesn't win here, both from running workers as processes:
- Bun's `cpu/req` is low because its extra processes get no connections on macOS (no `SO_REUSEPORT` balancing): it is really one process, with bigger event batches. Barm at one worker (4.8 µs) beats it.
- Four worker processes plus a supervisor cost about 2 MB of RSS each, more than one multithreaded Rust process. On Linux, where PSS counts shared pages once, Barm is lowest (2.5 MB vs 5.0 MB).

Pipelined, one worker:

| route | Barm | Rust | Bun | Node |
|---|---:|---:|---:|---:|
| hello | **2.26M** | 781k | 34k | 190k |
| json | **1.90M** | 702k | 33k | 186k |
| echo | **2.08M** | 535k | 229k | 133k |

## How Barm gets there

- **Native core, Barm API.** The event loop, HTTP/1.1 parser and response writer are C in the runtime (`runtime/barm.c`, `bm_native_http*`). Bun's API is Barm code (`crates/barm/src/std/http.barm`): `Bun.serve` compiles the routes, registers a closure with the loop, and the closure routes each request and writes the returned `Response`.
- **Shared buffers.** Each event loop reads into one 64 KB buffer and writes responses from one shared buffer; a connection keeps buffers of its own only for leftovers (half a request, or output the socket didn't take). An idle keep-alive connection is ~100 bytes, the working set stays in cache, and 1,000 connections add about 0.5 MB.
- **Cheap URLs.** A `URL` is kept as its normalized href and its parts are sliced when read; an already-normal URL (every request URL) is the input string itself. `new URL(req.url).pathname` costs three small allocations.
- **Level-triggered events, no wasted syscalls.** When a `read` fills less than the buffer, the socket is known to be drained, so the loop skips the extra `read` that would only return `EAGAIN`. A non-pipelined request costs one `read` and one `write`. With profiling on macOS, over 95% of server time is in those two syscalls.
- **Batched output.** Every response produced from one read goes out in one `write`, which is why pipelined throughput is 3–5× Rust's. hyper, under axum's defaults, flushes after each response (`pipeline_flush` is off).
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
bench/http/linux.sh --workers 1 --conns 128 --rate 200000 --routes json   # latency at a fixed load
out/load -c 64 -t 4 -d 5 http://127.0.0.1:3000/         # the load generator alone (-P pipeline, -R rate, -m/-b for POST)
```

`linux.sh` does the following:
- Emits the Barm server as C and compiles it in the container.
- Cross-compiles the Rust server with `zig cc`. rustc's aarch64 `--fix-cortex-a53-843419` link flag is filtered out, because zig doesn't know it.
- Copies `bun` out of the `oven/bun` image.

Results are written to `bench/results/http[-linux]-<timestamp>.json`.

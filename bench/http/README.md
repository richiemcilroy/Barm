# HTTP server benchmark

The same three routes served by Tov, Rust (axum on tokio), Bun and Node (`node:http`), driven by a keep-alive load generator. The Tov server is the Bun server's own code — `Bun.serve`, `new URL(req.url).pathname`, `Response.json` — with a `try` added where a call can throw: it measures idiomatic code, not a hand-tuned path.

| route | request | response |
|---|---|---|
| `hello` | `GET /` | `Hello, World!` (text/plain) |
| `json` | `GET /json` | `{"id":1,"name":"Ada Lovelace","email":"ada@example.com"}`, serialized per request |
| `echo` | `POST /echo` with a 75-byte JSON body | the body |

Files:
- `server.tov.ts` is the Tov server (`diff server.bun.ts server.tov.ts` shows the edits).
- `rust/` is the Rust server (axum 0.8, tokio 1, LTO, `panic = "abort"`).
- `server.bun.ts` is the Bun server, and `server.node.mjs` is the Node server.
- `load.c` is the load generator.
- `run.py` is the runner.
- `linux.sh` runs the benchmark on Linux in Docker.

## Results

Apple M4 Max (12 performance + 4 efficiency cores). The load generator runs on the same machine as the server, with 8 threads and 128 persistent connections, one request in flight per connection. Servers are interleaved across 3 repetitions and the median is shown. `cpu/req` is the server's CPU time per request. `mem` is the memory the server costs the machine: the proportional set size (PSS) on Linux and the physical footprint on macOS, summed over its processes, so pages that forked workers share are counted once. `rss` (resident set, summed) is in the JSON results too. It counts shared pages once per process, which inflates multi-process servers: four Tov workers and their supervisor each map the same libc pages, so summed RSS is 8.7 MB, against 1.7 MB of memory actually used (PSS). One multithreaded Rust process maps them once. PSS is the measure that adds up to what the machine spends.

### Linux (Docker VM, 16 vCPUs, epoll)

One process or thread:

| route | Tov | Rust (axum) | Bun |
|---|---:|---:|---:|
| hello | **385k req/s** | 311k | 278k |
| json | **371k** | 294k | 257k |
| echo | **372k** | 275k | 212k |
| cpu/req (json) | **1.63 µs** | 2.19 µs | 2.70 µs |
| mem (json) | **0.7 MB** | 4.9 MB | 39 MB |

Four workers. Tov and Bun use 4 processes. For Rust:
- `rust` is tokio's multi-thread runtime with 4 workers, the axum default.
- `rust-tpc` is thread-per-core: 4 current-thread runtimes with `SO_REUSEPORT` listeners, the layout the fastest Rust servers use.

| route | Tov | Rust tpc | Rust | Bun |
|---|---:|---:|---:|---:|
| hello | **1.65M req/s** | 1.38M | 731k | 1.05M |
| json | **1.61M** | 1.30M | 729k | 958k |
| echo | **1.60M** | 1.13M | 780k | 758k |
| cpu/req (json) | **2.44 µs** | 2.86 µs | 3.14 µs | 3.58 µs |
| mem (json) | **1.6 MB** | 5.1 MB | 6.3 MB | 97 MB |

Pipelined (16 requests in flight per connection, as in TechEmpower's plaintext test):

| | Tov | Rust tpc | Rust | Bun |
|---|---:|---:|---:|---:|
| hello, 1 worker | **3.87M req/s** | 773k | 798k | 52k |
| json, 1 worker | **2.37M** | 620k | 669k | 52k |
| hello, 4 workers | **10.4M** | 3.1M | 2.3M | 211k |
| json, 4 workers | **8.1M** | 2.4M | 2.2M | 203k |

#### Latency at a fixed load

The tables above are closed loop: each server runs as fast as it can, so tail latency there mixes in how hard the machine is being pushed. To compare latency, the load generator also runs open loop (`--rate`, like wrk2): requests go out on a fixed schedule every server can sustain, and each is timed from when it *should* have been sent.

`rust-nodelay` is Rust with `TCP_NODELAY` set (thread-per-core on four workers), so it sends each response at once, as Tov does. axum leaves Nagle's algorithm on by default.

1 worker at 200k req/s, json route:

| | Tov | Rust | Rust tpc | Rust, no delay |
|---|---:|---:|---:|---:|
| p50 | **346 µs** | 1.13 ms | 1.14 ms | 348 µs |
| p99 | **1.33 ms** | 2.07 ms | 2.07 ms | 1.46 ms |
| p99.9 | 1.96 ms | 2.71 ms | 2.62 ms | **1.81 ms** |
| cpu/req | 2.22 µs | 1.83 µs | **1.72 µs** | 2.89 µs |

4 workers at 600k req/s, json route:

| | Tov | Rust | Rust tpc | Rust, no delay |
|---|---:|---:|---:|---:|
| p50 | **190 µs** | 499 µs | 440 µs | 206 µs |
| p99 | **1.26 ms** | 2.10 ms | 1.92 ms | 1.70 ms |
| p99.9 | **2.18 ms** | 3.37 ms | 2.35 ms | 3.09 ms |
| cpu/req | 2.20 µs | 1.92 µs | **1.55 µs** | 2.76 µs |

Bun can't hold these rates open loop (it falls to 41k and 102k): when requests queue on a connection they arrive pipelined, where Bun is slow (see the pipelined table).

Tov sends every response the moment it's ready. Sending immediately costs more CPU per request than holding a response back:
- Rust with Nagle's algorithm (axum's default) holds a response while the client hasn't yet acknowledged the previous one. The response goes out when the acknowledgement arrives.
- On loopback, whoever sends a packet also pays for the receiver's TCP processing and wakeup. A held response is sent from the client's side, so its cost lands on the load generator, not on the server.
- The hold is also why Rust's median latency is 2.5–3.3 times Tov's.
- `perf` shows it directly. In Tov, transmitting is 45% of its CPU time, the client's receive and wakeup included. In default Rust, transmitting is 1.6%.

When Rust sends immediately too (`Rust, no delay`), its latency matches Tov's and it uses 25–30% more CPU per request. On a real network the receiver's TCP work runs on the client machine, and Tov uses about half Rust's user-space CPU per request.

### macOS (kqueue)

Here every server runs into the loopback cap of about 200k round trips/s (see Caveats), so req/s mostly shows who reaches the cap first. CPU per request is the better comparison. Memory is the physical footprint summed over the server's processes.

One process or thread:

| route | Tov | Rust (axum) | Bun | Node |
|---|---:|---:|---:|---:|
| hello | 196k · 5.07 µs | **203k · 4.96 µs** | 174k · 5.91 µs | 112k · 9.02 µs |
| json | **200k · 5.01 µs** | 195k · 5.15 µs | 164k · 6.30 µs | 108k · 9.33 µs |
| echo | **200k · 5.01 µs** | 177k · 5.65 µs | 147k · 7.01 µs | 94k · 10.8 µs |
| mem (json) | **1.6 MB** | 6.8 MB | 17 MB | 66 MB |

Four workers:

| route | Tov | Rust (axum) | Bun | Node |
|---|---:|---:|---:|---:|
| hello | **178k · 8.35 µs** | 164k · 12.6 µs | 171k · 6.06 µs | 160k · 16.1 µs |
| json | **176k · 8.51 µs** | 163k · 12.8 µs | 166k · 6.27 µs | 158k · 16.3 µs |
| echo | **176k · 8.56 µs** | 160k · 15.1 µs | 148k · 6.95 µs | 154k · 17.8 µs |
| mem (json) | **7.3 MB** | 7.9 MB | 42 MB | 209 MB |

Rows Tov doesn't win here:
- Bun's four-worker `cpu/req` is low because its extra processes get no connections on macOS (no `SO_REUSEPORT` balancing): it is really one process, with bigger event batches. Tov at one worker (5.0 µs) beats it.
- One-worker `hello` is within noise of Rust (±2% between runs); Tov wins `json` and `echo`.
- Four worker processes plus a supervisor are each about 1.4 MB, most of it what macOS charges any process (an idle C program is 1.2 MB). That still comes in under one multithreaded Rust process.

Pipelined, one worker:

| route | Tov | Rust | Bun | Node |
|---|---:|---:|---:|---:|
| hello | **2.26M** | 781k | 34k | 190k |
| json | **1.90M** | 702k | 33k | 186k |
| echo | **2.08M** | 535k | 229k | 133k |

## How Tov gets there

- **Native core, Tov API.** The event loop, HTTP/1.1 parser and response writer are C in the runtime (`runtime/tov.c`, `tv_native_http*`). Bun's API is Tov code (`crates/tov/src/std/http.tov`): `Bun.serve` compiles the routes, registers a closure with the loop, and the closure routes each request and writes the returned `Response`.
- **Shared buffers.** Each event loop reads into one 64 KB buffer (only the pages a read touches are resident) and writes responses from one shared buffer; a connection keeps buffers of its own only for leftovers (half a request, or output the socket didn't take). An idle keep-alive connection is ~100 bytes, the working set stays in cache, and 1,000 connections add about 0.5 MB.
- **Cheap URLs.** A `URL` is kept as its normalized href and its parts are sliced when read; an already-normal URL (every request URL) is the input string itself. `new URL(req.url).pathname` costs three small allocations.
- **Level-triggered events, no wasted syscalls.** When a `read` fills less than the buffer, the socket is known to be drained, so the loop skips the extra `read` that would only return `EAGAIN`. A non-pipelined request costs one `read` and one `write`. With profiling on macOS, over 95% of server time is in those two syscalls.
- **Batched output.** Every response produced from one read goes out in one `write`, which is why pipelined throughput is 3–5× Rust's. hyper, under axum's defaults, flushes after each response (`pipeline_flush` is off).
- **Cheap requests.** A request is a few small strings (and the builders that make its response) from the runtime's small-object free lists, a `Request` object and a `Response` object, all reference-counted and freed as soon as the handler returns. No GC, and no per-request futures or tasks. The `date` header is formatted once per second.
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
- **Tov features.** Tov's server has no TLS, HTTP/2, streaming bodies or async handlers yet. Handlers run to completion one at a time per worker.

## Running

```sh
python3 bench/http/run.py                               # macOS/Linux host: tov, rust, bun, node
python3 bench/http/run.py --workers 1 --routes json     # a subset
python3 bench/http/run.py --pipeline 16 --repeat 1
bench/http/linux.sh --workers 1,4 --threads 8 --conns 128   # Linux in Docker (gcc:14, oven/bun:1.4.0)
bench/http/linux.sh --workers 1 --conns 128 --rate 200000 --routes json   # latency at a fixed load
out/load -c 64 -t 4 -d 5 http://127.0.0.1:3000/         # the load generator alone (-P pipeline, -R rate, -m/-b for POST)
```

`linux.sh` does the following:
- Emits the Tov server as C and compiles it in the container.
- Cross-compiles the Rust server with `zig cc`. rustc's aarch64 `--fix-cortex-a53-843419` link flag is filtered out, because zig doesn't know it.
- Copies `bun` out of the `oven/bun` image.

Results are written to `bench/results/http[-linux]-<timestamp>.json`.

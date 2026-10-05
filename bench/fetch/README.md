# fetch() client benchmark

The same client program calls one server from Tov, Bun, Node and Rust, and each client's throughput, CPU time and memory is measured.

- `client.tov` is the client. Tov compiles it natively, and Bun and Node run it as TypeScript (the runner strips Tov's `try` markers and `throws` clauses).
- `rust-client/` is the Rust client: reqwest 0.12 on hyper. `rust` runs it on a current-thread tokio runtime, one core like the others. `rust-mt` uses tokio's multi-thread runtime.
- `server/` is the server: axum on a 4-thread tokio runtime, fast enough that it never limits a client.
- `run.py` is the runner: `python3 bench/fetch/run.py [--only tov,bun] [--scenarios hello-1] [--reps 3]`.

| scenario | work |
|---|---|
| `hello-1` | 20,000 `GET /` (`Hello, World!`), one at a time: per-request latency |
| `hello-64` | 100,000 `GET /`, 64 loops at once |
| `json-64` | 100,000 `GET /json`, each parsed (`res.json()`), 64 at once |
| `echo-64` | 100,000 `POST /echo` with a 75-byte JSON body, read back with `text()`, 64 at once |
| `big-1` | 100 downloads of 8 MiB, read with `text()`, one at a time |
| `big-8` | 200 downloads of 8 MiB, 8 at once |
| `gzip-1` | 100 downloads of 8 MiB of word-like text sent gzipped (3.8 MiB), decoded by `fetch()` and read with `text()` |
| `tls-hello-1` | as `hello-1`, over HTTPS (TLS 1.3, one pooled connection) |
| `tls-hello-64` | as `hello-64`, over HTTPS (64 connections) |
| `tls-new-1` | 2,000 `GET /` over HTTPS with `Connection: close`: a new connection and handshake each time (sessions resume) |
| `tls-big-1` | 100 downloads of 8 MiB over HTTPS |

## Results

Apple M4 Max, macOS, loopback. Each number is the median of 3 runs with clients interleaved. `cpu/request` is the client's user + system CPU time divided by requests; `peak RSS` is the client's maximum resident set.

| scenario | **Tov** | Bun 1.4 | Node 26 (undici) | Rust (reqwest) | Rust, multi-thread |
|---|---:|---:|---:|---:|---:|
| hello-1 | **45.7k req/s** | 24.9k | 15.2k | 34.8k | 34.6k |
| cpu/request | **10.2 µs** | 37.5 µs | 80.9 µs | 14.8 µs | 15.0 µs |
| peak RSS | **2.5 MB** | 39.4 MB | 222 MB | 3.9 MB | 4.5 MB |
| hello-64 | **179k req/s** | 128k | 31.3k | 130k | 145k |
| cpu/request | **5.5 µs** | 13.0 µs | 47.1 µs | 7.6 µs | 31.3 µs |
| peak RSS | **3.1 MB** | 41.5 MB | 251 MB | 8.3 MB | 11.0 MB |
| json-64 | **180k req/s** | 131k | 30.8k | 128k | 144k |
| cpu/request | **5.5 µs** | 12.7 µs | 47.6 µs | 7.8 µs | 31.7 µs |
| peak RSS | **3.2 MB** | 39.4 MB | 242 MB | 8.4 MB | 11.2 MB |
| echo-64 | **170k req/s** | 128k | 20.4k | 120k | 143k |
| cpu/request | **5.8 µs** | 13.1 µs | 69.2 µs | 8.1 µs | 32.3 µs |
| peak RSS | **3.2 MB** | 41.6 MB | 249 MB | 8.4 MB | 11.1 MB |
| big-1 | **1.03k req/s** (8.6 GB/s) | 0.92k | 0.25k | 0.20k | 0.20k |
| cpu/request | **758 µs** | 1,372 µs | 6,416 µs | 3,793 µs | 4,012 µs |
| peak RSS | **10.8 MB** | 129 MB | 214 MB | 37.7 MB | 39.2 MB |
| big-8 | **1.29k req/s** | 1.18k | 0.33k | 0.33k | 0.70k |
| cpu/request | **746 µs** | 1,361 µs | 4,834 µs | 3,026 µs | 4,447 µs |
| peak RSS | **70.9 MB** | 196 MB | 513 MB | 135 MB | 236 MB |
| gzip-1 | **78.9 req/s** (662 MB/s decoded) | 56.2 | 42.4 | 33.9 | 33.7 |
| cpu/request | **11,532 µs** | 18,340 µs | 30,096 µs | 29,308 µs | 30,170 µs |
| peak RSS | **15.0 MB** | 198 MB | 209 MB | 32.9 MB | 32.8 MB |

HTTPS: the server is the same axum app behind rustls, using a certificate from `tests/fetch/tls` that every client trusts through `NODE_EXTRA_CA_CERTS`. Tov uses BoringSSL, as Bun does.

| scenario | **Tov** | Bun 1.4 | Node 26 | Rust (reqwest + rustls) | Rust, multi-thread |
|---|---:|---:|---:|---:|---:|
| tls-hello-1 | **39.4k req/s** | 24.2k | 15.5k | 35.3k | 34.8k |
| cpu/request | **11.7 µs** | 38.6 µs | 80.8 µs | 14.7 µs | 15.1 µs |
| peak RSS | **4.4 MB** | 42.7 MB | 227 MB | 4.8 MB | 5.4 MB |
| tls-hello-64 | **162k req/s** | 116k | 28.3k | 118k | 128k |
| cpu/request | **6.1 µs** | 14.1 µs | 50.4 µs | 9.6 µs | 38.3 µs |
| peak RSS | **6.9 MB** | 44.5 MB | 259 MB | 42.4 MB | 93.5 MB |
| tls-new-1 | **5.1k req/s** | 4.6k | 0.9k | 2.7k | 2.7k |
| cpu/request | **111 µs** | 147 µs | 966 µs | 201 µs | 213 µs |
| peak RSS | **4.7 MB** | 26.2 MB | 163 MB | 5.1 MB | 5.6 MB |
| tls-big-1 | **0.40k req/s** (3.4 GB/s) | 0.36k | 0.14k | 0.19k | 0.19k |
| cpu/request | **2,402 µs** | 3,644 µs | 10,084 µs | 4,946 µs | 5,603 µs |
| peak RSS | **12.7 MB** | 123 MB | 217 MB | 42.8 MB | 42.9 MB |

In `tls-new-1`, Tov offers the post-quantum X25519+ML-KEM-768 key exchange, as Chrome, Firefox and Node do; Bun offers plain X25519. The post-quantum share costs the client about 40 µs per new connection (pooled connections don't pay it), and Tov still leads: it builds the ClientHello, key shares included, while the TCP handshake is in flight, and it compiles BoringSSL as a release build (`-O3 -DNDEBUG`).

Binary size: a Tov program that calls `fetch()` is 1.9 MB, TLS included (hello-world is 35 KB: programs that don't fetch don't link TLS). The Rust client with reqwest and rustls is 2.4 MB, and Bun is 64 MB.

In `big-8`, Tov is 9% ahead of Bun on half the CPU and about a third of the memory. Bun spreads work over several threads (its CPU time exceeds its wall time).

## What makes it fast

- The client runs natively on the event loop. There are no per-request threads, and a request costs a few small allocations.
- Connections are pooled per origin and reused most-recently-first. A request is one `sendmsg`, and the response is parsed incrementally from the socket.
- A body with a `Content-Length` is read straight into the buffer that becomes the string: one copy, from the kernel.
- A compressed body read whole is decoded in one pass when it's all in, by libdeflate (twice as fast as zlib) into a buffer reserved once. One read in chunks (`res.body`) is decoded as it arrives, by zlib, at most 4 MB ahead of the reader.
- `text()` doesn't read the body again. Each chunk is checked as UTF-8 while it is still in cache, as it arrives. A second pass over an 8 MiB body evicted the cache the next transfer needed, and made downloads 4× slower.

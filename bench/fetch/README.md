# fetch() client benchmark

The same client program calls one server from Barm, Bun, Node and Rust, and each client's throughput, CPU time and memory is measured.

- `client.barm` is the client. Barm compiles it natively, and Bun and Node run it as TypeScript (the runner strips Barm's `try` markers and `throws` clauses).
- `rust-client/` is the Rust client: reqwest 0.12 on hyper. `rust` runs it on a current-thread tokio runtime, one core like the others. `rust-mt` uses tokio's multi-thread runtime.
- `server/` is the server: axum on a 4-thread tokio runtime, fast enough that it never limits a client.
- `run.py` is the runner: `python3 bench/fetch/run.py [--only barm,bun] [--scenarios hello-1] [--reps 3]`.

| scenario | work |
|---|---|
| `hello-1` | 20,000 `GET /` (`Hello, World!`), one at a time: per-request latency |
| `hello-64` | 100,000 `GET /`, 64 loops at once |
| `json-64` | 100,000 `GET /json`, each parsed (`res.json()`), 64 at once |
| `echo-64` | 100,000 `POST /echo` with a 75-byte JSON body, read back with `text()`, 64 at once |
| `big-1` | 100 downloads of 8 MiB, read with `text()`, one at a time |
| `big-8` | 200 downloads of 8 MiB, 8 at once |

## Results

Apple M4 Max, macOS, loopback. Each number is the median of 3 runs with clients interleaved. `cpu/request` is the client's user + system CPU time divided by requests; `peak RSS` is the client's maximum resident set.

| scenario | **Barm** | Bun 1.4 | Node 26 (undici) | Rust (reqwest) | Rust, multi-thread |
|---|---:|---:|---:|---:|---:|
| hello-1 | **46.8k req/s** | 25.1k | 15.6k | 35.1k | 35.0k |
| cpu/request | **9.9 µs** | 37.2 µs | 78.4 µs | 14.7 µs | 14.9 µs |
| peak RSS | **2.4 MB** | 39.4 MB | 221 MB | 3.6 MB | 4.1 MB |
| hello-64 | **180k req/s** | 124k | 31.0k | 128k | 146k |
| cpu/request | **5.5 µs** | 13.3 µs | 47.5 µs | 7.8 µs | 29.7 µs |
| peak RSS | **2.8 MB** | 41.6 MB | 251 MB | 7.8 MB | 10.3 MB |
| json-64 | **187k req/s** | 129k | 30.8k | 129k | 146k |
| cpu/request | **5.3 µs** | 12.9 µs | 47.5 µs | 7.7 µs | 29.5 µs |
| peak RSS | **2.8 MB** | 39.8 MB | 252 MB | 7.4 MB | 10.5 MB |
| echo-64 | **178k req/s** | 128k | 20.3k | 126k | 144k |
| cpu/request | **5.6 µs** | 13.1 µs | 69.2 µs | 7.9 µs | 30.9 µs |
| peak RSS | **2.9 MB** | 41.5 MB | 249 MB | 8.0 MB | 10.4 MB |
| big-1 | **1.0k req/s** (8.4 GB/s) | 0.9k | 0.2k | 0.2k | 0.2k |
| cpu/request | **752 µs** | 1,372 µs | 6,651 µs | 3,810 µs | 4,011 µs |
| peak RSS | **10.7 MB** | 141 MB | 234 MB | 37.6 MB | 39.6 MB |
| big-8 | **1.2k req/s** | 1.2k | 0.3k | 0.3k | 0.7k |
| cpu/request | **753 µs** | 1,379 µs | 4,918 µs | 3,027 µs | 4,455 µs |
| peak RSS | **70.8 MB** | 187 MB | 465 MB | 129 MB | 244 MB |

In `big-8`, Barm and Bun reach the same throughput; Barm uses about half the CPU and a third of the memory. Bun spreads work over several threads (its CPU time exceeds its wall time).

## What makes it fast

- The client runs natively on the event loop. There are no per-request threads, and a request costs a few small allocations.
- Connections are pooled per origin and reused most-recently-first. A request is one `sendmsg`, and the response is parsed incrementally from the socket.
- A body with a `Content-Length` is read straight into the buffer that becomes the string: one copy, from the kernel.
- `text()` doesn't read the body again. Each chunk is checked as UTF-8 while it is still in cache, as it arrives. A second pass over an 8 MiB body evicted the cache the next transfer needed, and made downloads 4× slower.

# Cap's media server: Bun vs Tov

[Cap's media server](https://github.com/CapSoftware/Cap/tree/main/apps/media-server) is a Bun app: Hono routes, zod, mediabunny with node-av (an N-API addon wrapping FFmpeg), `Bun.spawn` for ffmpeg subprocesses, `Bun.file`, and Bun's convention of serving an entry point's default export. Cap runs it as `bun src/index.ts`.

Tov runs the same source, unchanged. `out/ms/main.tov` is the whole port:

```ts
import server from "cap-media-server"
import { serve } from "bun"

function main() {
  // Cap's media server, as Bun runs it: its default export served
  try serve(server)
}
```

`cap-media-server` is a one-line package re-exporting `src/index.ts`. `bun` is Tov's `bun` module (`runtime/node/bun.js`): `serve` on Tov's native HTTP server, `spawn` over `child_process`, and `file` as a Blob read from its file.

## What's measured

`run.py` copies the media server out of a Cap checkout, installs its packages with `bun install --production`, and builds the Tov binary. It then serves a generated test video (10 s, 1280×720 H.264 with AAC) over local HTTP. Each runtime is started in turn, interleaved across repetitions, and for each:

| metric | how |
|---|---|
| `startup_ms` | from exec to the first `200` from `/health` |
| `idle_mb` | physical footprint 3 s after startup |
| `health_rps`, `health_p50_us`, `health_p99_us` | `GET /health` with 64 connections (`bench/http/load.c`) |
| `probe_ops`, `probe_p50_ms`, `probe_p99_ms` | `POST /video/probe` (node-av reads the video over HTTP), 4 in flight (the server takes 6 at once) |
| `extract_ops`, `extract_p50_ms`, `extract_p99_ms` | `POST /audio/extract` (an ffmpeg subprocess, its output streamed back), 2 in flight |
| `health_cpu_us`, `probe_cpu_us`, `extract_cpu_us` | the server's own CPU time (user + system, not its children's) per completed request |
| `after_mb`, `peak_mb` | physical footprint 3 s after the load, and its peak |
| `shutdown_ms` | from SIGTERM to exit (the server's own shutdown handler runs) |

The media routes are driven by `client.mjs` under Node, so the client is the same whichever runtime serves. `/video/thumbnail` isn't measured: with FFmpeg 8 its `image2` pipe output fails (exit 234) under both runtimes.

```
bench/cap-media/run.py --cap ~/github/Cap --reps 3 --secs 10
```

## Results

Bun 1.4.0, Tov on its own JavaScriptCore (`scripts/jsc`). Medians of 3 repetitions with 10 s per load: on macOS from `bench/results/cap-media-20261011-013803.json`, and a run on Linux. The origin and the load generator run on the same machine, so differences of a few percent are noise. Memory is the physical footprint on macOS and the resident set on Linux.

| | macOS (M4 Max): Bun | Tov | Linux (x86-64, Ryzen 9950X): Bun | Tov |
|---|---:|---:|---:|---:|
| startup (exec to first `/health`) | 128 ms | **107 ms** | **193 ms** | 354 ms |
| idle memory | 50 MB | 50 MB | **91 MB** | 125 MB |
| `/health` | 33.1k req/s (p99 3.7 ms) | **35.5k req/s** (p99 3.4 ms) | 21.4k req/s (p99 6.3 ms) | **22.5k req/s** (p99 4.6 ms) |
| CPU per `/health` | 34 µs | **32 µs** | 53 µs | **51 µs** |
| `/video/probe` | **1,468/s** (p99 7.4 ms) | 1,353/s (p99 7.7 ms) | 517/s (p99 66 ms) | **899/s** (p99 12 ms) |
| CPU per probe | 1,248 µs | **881 µs** | 2,708 µs | **1,349 µs** |
| `/audio/extract` | 19.6/s (p99 172 ms) | 19.0/s (p99 154 ms) | 17.9/s (p99 249 ms) | 16.9/s (p99 167 ms) |
| CPU per extract (the server's, not ffmpeg's) | 10.4 ms | **10.2 ms** | **8.6 ms** | 13.2 ms |
| memory 3 s after the load | 65 MB | 66 MB | **137 MB** | 148 MB |
| peak memory | 136 MB | **126 MB** | **162 MB** | 205 MB |
| failed requests | 0 | 0 | 0 | 0 |

The Tov binary builds in 0.8 s on macOS with its bytecode cached (1.3 s with nothing cached) and is 36 MB with its engine; on Linux, 47 MB. The packages' native addon (node-av, 61 MB) loads from `node_modules`, as under Bun.

- **CPU**: Tov probes a video on 29% less CPU than Bun on macOS, and half on Linux.
- **Throughput**: on macOS, with 4 probes in flight, Bun completes 8% more each second. Its `fetch()` client runs on a thread of its own, so the server's thread and the client's work in parallel; Tov's client runs on the event loop. On Linux Tov completes 74% more.
- **Startup**: on macOS Tov loads the server's 654 modules from its bytecode cache, written the first time the program idles. On Linux it starts slower than Bun, and holds more memory throughout: the engine's Linux build isn't tuned as the macOS one is yet.
- **Memory**: on macOS Tov peaks lower under load: its engine lets the heap grow to about twice what survives a collection, where JavaScriptCore's default on a machine with 16 GB or more is about four times. Once the server has been idle a second it collects and returns memory (a timer that does next to nothing, like the 5 s timeout the server leaves per ffmpeg it runs, doesn't count as work), and drops compiled code soon after.
- **Reliability**: when the origin refuses a connection under load, mediabunny's retried fetch rejects. Bun 1.4.0 has exited on it in earlier runs; Tov reports the request's failure and carries on.

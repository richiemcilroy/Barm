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

Apple M4 Max, macOS, Bun 1.4.0, Tov on its own JavaScriptCore (`scripts/jsc`). Medians of 3 repetitions with 10 s per load, from `bench/results/cap-media-20261005-131621.json`. The origin and the load generator run on the same machine, which was busy (load average above 10), so differences of a few percent are noise.

| | Bun | Tov |
|---|---:|---:|
| startup (exec to first `/health`) | 120 ms | **105 ms** |
| idle memory | 50 MB | 51 MB |
| `/health` | **35.3k req/s** (p99 3.5 ms) | 33.3k req/s (p99 3.7 ms) |
| `/video/probe` | **2,409/s** (p99 4.0 ms) | 2,084/s (p99 4.4 ms) |
| CPU per probe | 794 µs | **579 µs** |
| `/audio/extract` | 25.5/s (p99 88 ms) | 25.5/s (p99 86 ms) |
| CPU per extract (the server's, not ffmpeg's) | 8.4 ms | **8.1 ms** |
| memory after the load | **64 MB** | 76 MB |
| peak memory | 143 MB | **129 MB** |
| failed requests | 0 | 0 |

The Tov binary builds in 3.8 s and is 34 MB with its engine (on the system's JavaScriptCore, `TOV_JSC=system`: 1.3 s and 8.4 MB), plus the packages' native addon (node-av, 61 MB, loaded from `node_modules` as under Bun).

- **CPU**: Tov probes a video on 27% less CPU than Bun.
- **Throughput**: with 4 probes in flight Bun completes 16% more each second. Its `fetch()` client runs on a thread of its own, so the server's thread and the client's work in parallel; Tov's client runs on the event loop. `/health` (Hono, zod, `os` and `process` metrics) is 6% faster on Bun; on the system's JavaScriptCore Tov matched it.
- **Startup**: Tov loads the server's 654 modules from its bytecode cache, written the first time the program idles.
- **Memory**: Tov peaks lower under load: its engine lets the heap grow to about twice what survives a collection, where JavaScriptCore's default on a machine with 16 GB or more is about four times. After a burst Bun gives memory back sooner; Tov collects and returns memory once the server has been idle a second, and drops compiled code after ten.
- **Reliability**: when the origin refuses a connection under load, mediabunny's retried fetch rejects. Bun 1.4.0 has exited on it in earlier runs; Tov reports the request's failure and carries on.

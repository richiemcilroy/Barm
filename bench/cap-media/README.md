# Cap's media server: Bun vs Barm

[Cap's media server](https://github.com/CapSoftware/Cap/tree/main/apps/media-server) is a Bun app: Hono routes, zod, mediabunny with node-av (an N-API addon wrapping FFmpeg), `Bun.spawn` for ffmpeg subprocesses, `Bun.file`, and Bun's convention of serving an entry point's default export. Cap runs it as `bun src/index.ts`.

Barm runs the same source, unchanged. `out/ms/main.barm` is the whole port:

```ts
import server from "cap-media-server"
import { serve } from "bun"

function main() {
  // Cap's media server, as Bun runs it: its default export served
  try serve(server)
}
```

`cap-media-server` is a one-line package re-exporting `src/index.ts`. `bun` is Barm's `bun` module (`runtime/node/bun.js`): `serve` on Barm's native HTTP server, `spawn` over `child_process`, and `file` as a Blob read from its file.

## What's measured

`run.py` copies the media server out of a Cap checkout, installs its packages with `bun install --production`, and builds the Barm binary. It then serves a generated test video (10 s, 1280×720 H.264 with AAC) over local HTTP. Each runtime is started in turn, interleaved across repetitions, and for each:

| metric | how |
|---|---|
| `startup_ms` | from exec to the first `200` from `/health` |
| `idle_mb` | physical footprint 3 s after startup |
| `health_rps`, `health_p50_us`, `health_p99_us` | `GET /health` with 64 connections (`bench/http/load.c`) |
| `probe_ops`, `probe_p50_ms`, `probe_p99_ms` | `POST /video/probe` (node-av reads the video over HTTP), 4 in flight (the server takes 6 at once) |
| `extract_ops`, `extract_p50_ms`, `extract_p99_ms` | `POST /audio/extract` (an ffmpeg subprocess, its output streamed back), 2 in flight |
| `after_mb`, `peak_mb` | physical footprint 3 s after the load, and its peak |
| `shutdown_ms` | from SIGTERM to exit (the server's own shutdown handler runs) |

The media routes are driven by `client.mjs` under Node, so the client is the same whichever runtime serves. `/video/thumbnail` isn't measured: with FFmpeg 8 its `image2` pipe output fails (exit 234) under both runtimes.

```
bench/cap-media/run.py --cap ~/github/Cap --reps 3 --secs 10
```

## Results

Apple M4 Max, macOS, Bun 1.4.0. Medians of 3 repetitions with 10 s per load, from `bench/results/cap-media-20261002-154859.json`. The origin and the load generator run on the same machine.

| | Bun | Barm |
|---|---:|---:|
| startup (exec to first `/health`) | 113 ms | **97 ms** |
| idle memory | **50 MB** | 52 MB |
| `/health` | 38.2k req/s (p99 3.2 ms) | **38.9k req/s** (p99 3.1 ms) |
| `/video/probe` | 315/s (p99 19 ms) | 316/s (p99 31 ms) |
| `/audio/extract` | 23.7/s (p99 110 ms) | 23.7/s (p99 110 ms) |
| memory after the load | **65 MB** | 95 MB |
| peak memory | **158 MB** | 257 MB |
| failed requests | crashed in 1 of 3 runs | **0** |

The Barm binary builds in 1.3 s and is 8.4 MB, plus the packages' native addon (node-av, 61 MB, loaded from `node_modules` as under Bun).

- **Throughput**: even. The media routes' time is FFmpeg's (in node-av, and in ffmpeg subprocesses) and the origin's, which both runtimes share; `/health` (Hono, zod, `os` and `process` metrics) is a little faster on Barm.
- **Startup**: Barm loads the server's 654 modules from its bytecode cache, written the first time the program idles.
- **Reliability**: when the origin refuses a connection under load, mediabunny's retried fetch rejects. Bun exits on it (`Bun v1.4.0` after the error); Barm reports the request's failure and carries on.
- **Memory**: Barm holds more after a burst and at its peak. Most of the gap is the engine's heap: the probe route's demuxing is JavaScript over many short-lived ArrayBuffers, and Bun's JavaScriptCore and allocator (mimalloc) give that memory back sooner. Barm runs on the system's JavaScriptCore; it collects and returns memory once the server has been idle a second, and drops compiled code after ten.

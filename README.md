# Barm

**A programming language for coding agents.** Barm looks like TypeScript, so your agent already knows how to write it. The compiler checks every line and hands back the exact fix for each error. What comes out is a small native binary that keeps pace with Rust.

- **Nothing new to learn.** TypeScript's syntax with Bun's and Node's APIs: `Bun.serve`, `fetch`, `node:fs`, `async`/`await`. If code looks like TypeScript it behaves like TypeScript, or it doesn't compile.
- **Nothing hidden.** No `any`, `null` or `==`. Every call that can fail is marked `try`, integer overflow stops the program, and arrays and records are values, so nothing changes behind the agent's back.
- **Errors that come with the fix.** Every error has a code, a reason and a fix. `barm check --json` returns the exact edit and marks the ones that are safe to apply as they are.
- **Fast feedback, fast programs.** Builds take about 0.1 s. A web server is a 150 KB binary with no runtime to install, and it outruns Rust's axum and Bun.
- **Your npm packages still work.** Barm programs import npm packages, and apps written for Bun run on Barm unchanged.

The landing page, [site/](site/), is a Barm program: `cd site && barm dev`.

## Status

Barm is early: it's at milestone 3 of its spec, and it changes every day.

| Works today | Not yet |
|---|---|
| Command-line programs and web servers | Windows |
| Records, unions, generics, classes, interfaces, checked errors | A package manager of its own (install packages with npm or Bun) |
| `async`/`await`, promises, timers | Tuples, enums and some other TypeScript features ([spec](docs/spec.md)) |
| `fetch()` over HTTP and HTTPS, with streaming, compression and proxies | TLS sockets (`tls.connect`) and worker threads for npm packages |
| Bun's server API (`Bun.serve`, routes, `Request`, `Response`) | `Promise.allSettled`/`any`, `.then`/`.catch` |
| A Node-style standard library (`fs`, `path`, `process`) | |
| npm packages, including Node-API native addons, and Bun apps | |
| macOS (arm64) and Linux | |

## How an agent works with Barm

An agent writes TypeScript:

```ts
Bun.serve({
  port: 3000,
  fetch(req) {
    const url = new URL(req.url)
    return new Response(`Hello from ${url.pathname}`)
  },
})
```

Barm says what's wrong and how to fix it, for people:

```
error[T0831] hello.barm:4:17: `new URL` can throw `TypeError`; mark the call `try` to pass the error on, or catch it
    4 |     const url = new URL(req.url)
  why: errors are values in Barm: every call that can fail is marked, so control flow is visible
  or: wrap it in `try { ... } catch (e) { ... }`
  fix[safe]: pass it on: `try new URL(req.url)`
```

and for tools (`barm check --json`, trimmed):

```json
{
  "code": "T0831",
  "file": "hello.barm",
  "start": { "line": 4, "col": 17 },
  "message": "`new URL` can throw `TypeError`; mark the call `try` to pass the error on, or catch it",
  "fixes": [{
    "label": "pass it on: `try new URL(req.url)`",
    "applicability": "safe",
    "edits": [{ "start": { "line": 4, "col": 17 }, "end": { "line": 4, "col": 17 }, "text": "try " }]
  }]
}
```

A `safe` fix can be applied as it is, and `barm explain T0831` describes any code. With the fix applied, `barm build hello.barm` writes a 152 KB binary.

To get an agent started, point it at [docs/spec.md](docs/spec.md): it lists only what differs from TypeScript, so it's short.

## Example

```ts
type Shape =
  | { kind: "circle", r: f64 }
  | { kind: "rect", w: f64, h: f64 }

function area(s: Shape): f64 {
  switch (s.kind) {
    case "circle":
      return Math.PI * s.r * s.r
    case "rect":
      return s.w * s.h
  }
}

function main() {
  const shapes: Shape[] = [{ kind: "circle", r: 1 }, { kind: "rect", w: 2, h: 3 }]
  for (const s of shapes) {
    console.log(`${s.kind}: ${area(s).toFixed(2)}`)
  }
}

test("rect area", () => {
  expect(area({ kind: "rect", w: 2, h: 3 })).toBe(6)
})
```

## Getting started

You need Rust (to build the compiler) and a C compiler (`clang` or `gcc`).

```sh
git clone https://github.com/richiemcilroy/Barm
cd Barm && cargo install --path crates/barm   # puts `barm` in ~/.cargo/bin

barm run examples/shapes          # build and run main()
barm --watch server.barm          # rebuild and restart when a source file changes
barm dev                          # run package.json's "dev" script, as `bun dev` does
barm test examples                # run every test(...)
barm build examples/shapes -o app # write a native binary
barm check src/                   # type-check only (--json for tools)
barm explain T0001                # what an error code means
```

The command line works like Bun's: `barm <file>` runs a program and passes it the arguments after the file name, `barm <script>` runs a `package.json` script (with `pre`/`post` scripts, the `npm_*` environment and Bun's output), and `--watch` restarts the program when the entry file or anything it imports changes.

## How it differs from TypeScript

Barm keeps TypeScript's syntax and drops the parts that make programs hard to reason about or slow to run:

- No `any`, `null`, `==` or implicit type conversions.
- `int` and `f64` are separate types. Integer overflow traps instead of silently losing precision.
- Arrays, records and maps are values. Assigning one makes a copy, and the compiler flags code where TypeScript would have shared the value instead.
- To modify a caller's value, a function declares an `inout` parameter and the caller passes `&x`.
- Every call that can fail is marked `try` (or caught), and a function's errors are part of its type.
- Unions of object types must be narrowed before you use their fields, and a `switch` must cover every case.
- Classes are reference-counted, not garbage-collected. If instances of a class could reference each other in a cycle, the compiler asks you to mark the back-reference `weak`, make it `readonly`, or declare a `cyclic class` (which adds cycle collection for that class only).

The full list is in [docs/spec.md](docs/spec.md).

## npm packages and Bun apps

Install packages with npm or Bun, then import them:

```ts
import { z } from "zod"

const User = try z.object({ name: z.string(), age: z.number() })
const r = try User.safeParse({ name: "Ada", age: 36 })
console.log(r.success as bool)
```

Barm code compiles to native code. The packages are bundled when the program is built (resolved as Node.js resolves them; ES modules, TypeScript and JSX converted), embedded in the binary, and run on JavaScriptCore, the engine Bun uses. Values that come from JavaScript have the type `Js` and are checked when converted (`as bool`), and JavaScript calls that can throw are marked `try` like any other.

Node's built-in modules are there for packages that need them (`fs`, `http`, `net`, `child_process`, `crypto`, `zlib`, `dns`, streams and the rest), and so is Bun's (`Bun.serve`, `Bun.spawn`, `Bun.file`). Native addons load through Node-API. An app written for Bun runs as it is: this is the whole port of [Cap's media server](https://github.com/CapSoftware/Cap/tree/main/apps/media-server) (Hono, zod, FFmpeg through a native addon, `Bun.spawn`):

```ts
import server from "cap-media-server"
import { serve } from "bun"

function main() {
  // Cap's media server, as Bun runs it: its default export served
  try serve(server)
}
```

Programs that import npm packages link Barm's own build of JavaScriptCore when it's built (`scripts/jsc/build.sh`, about 10 minutes), or else the system's (macOS) or WebKitGTK's (Linux). The engine makes such a binary about 34 MB; programs that don't import npm packages don't include it.

## Performance

All numbers are from an Apple M4 Max. Each benchmark's README has its method, every result and its caveats.

### Programs

Median wall time and peak memory. Every language prints identical output ([bench/README.md](bench/README.md)).

| benchmark | C | Rust | Node | Bun | **Barm** |
|---|---:|---:|---:|---:|---:|
| binary_trees | 703 ms · 17.9 MB | 801 ms · 18.1 MB | 478 ms · 202 MB | 390 ms · 162 MB | **266 ms · 17.8 MB** |
| class_trees | 705 ms · 17.9 MB | 812 ms · 18.1 MB | 468 ms · 201 MB | 381 ms · 215 MB | **266 ms · 17.8 MB** |
| dispatch (virtual calls) | 83 ms · 1.8 MB | 83 ms · 2.0 MB | 211 ms · 81 MB | 113 ms · 25 MB | **39 ms** · 1.9 MB |
| map_insert | 471 ms · 225 MB | 508 ms · 297 MB | 900 ms · 282 MB | 742 ms · 564 MB | **253 ms · 150 MB** |
| nbody | 319 ms · 1.8 MB | 229 ms · 2.0 MB | 558 ms · 77 MB | 510 ms · 22 MB | **211 ms · 1.8 MB** |
| sort | 282 ms · 24.7 MB | 75 ms · 38.5 MB | 1166 ms · 266 MB | 697 ms · 116 MB | **47 ms** · 25.0 MB |
| strings | 261 ms · 206 MB | 228 ms · 251 MB | 505 ms · 664 MB | 203 ms · 568 MB | **136 ms · 159 MB** |
| array_push | 54 ms · 233 MB | 64 ms · 233 MB | 360 ms · 742 MB | 221 ms · 579 MB | **49 ms · 231 MB** |
| fib | 506 ms · 1.8 MB | 454 ms · 1.9 MB | 1505 ms · 75 MB | 970 ms · 18 MB | **425 ms · 1.8 MB** |
| points | **319 ms** · 1.8 MB | 320 ms · 1.9 MB | 1960 ms · 83 MB | 885 ms · 30 MB | 320 ms · 1.8 MB |

Barm checks for integer overflow by default (fib above); Rust with overflow checks takes 592 ms on fib. `points` is bound by floating-point add latency in all three compiled languages. Some differences are the libraries': Rust's `HashMap` hashes with SipHash (DoS-resistant and slower), and C's `qsort` calls its comparator through a function pointer.

Binaries are 34–35 KB (33 KB stripped, the same as C; Rust's are about 470 KB), and a build takes 0.04–0.07 s (C 0.04–0.05 s, Rust 0.08–0.16 s). Run them yourself with `python3 bench/run.py`.

### HTTP server

Barm speaks Bun's server API, so a Bun server ports with only Barm's usual edits (a `try` on calls that can throw). `tests/parity` runs a Bun server under Bun and its port under Barm and checks that every response matches.

```ts
type Todo = { id: number; title: string }
const todos: Todo[] = []

const server = Bun.serve({
  port: 3000,
  routes: {
    "/todos/:id": (req) => {
      const todo = todos.find((t) => t.id === Number(req.params.id))
      return todo === undefined ? new Response("Not Found", { status: 404 }) : Response.json(todo)
    },
    "/todos": {
      GET: () => Response.json(todos),
      POST: async (req) => {
        const todo = try (await req.json()) as Todo
        todos.push(todo)
        return Response.json(todo, { status: 201 })
      },
    },
  },
  fetch(req) {
    return new Response("Not Found", { status: 404 })
  },
})
console.log(`Listening on ${server.url}`)
```

Requests per second on Linux (Docker), 128 keep-alive connections, JSON route, with Barm running the Bun benchmark's own code ([bench/http/README.md](bench/http/README.md)):

| | Rust (axum) | Bun | **Barm** |
|---|---:|---:|---:|
| 1 core | 294k | 257k | **371k** |
| 4 cores | 1.30M\* | 958k | **1.61M** |
| p99 at a fixed 600k req/s, 4 cores | 1.91 ms\* | — | **1.30 ms** |
| memory, 4 cores (PSS) | 5.1 MB\* | 97 MB | **1.6 MB** |

\* Thread-per-core Rust; tokio's default multi-thread runtime reaches 720k. With pipelining, Barm serves 8.1M JSON requests/s on 4 cores (Rust 2.4M).

### HTTP client

`fetch()` against a local server, macOS ([bench/fetch/README.md](bench/fetch/README.md)):

| | **Barm** | Bun | Node (undici) | Rust (reqwest) |
|---|---:|---:|---:|---:|
| small requests, one at a time | **45.7k req/s** | 24.9k | 15.2k | 34.8k |
| small requests, 64 at once | **179k req/s** | 128k | 31.3k | 145k\* |
| CPU per request, 64 at once | **5.5 µs** | 13.0 µs | 47.1 µs | 7.6 µs |
| memory, 64 at once | **3.1 MB** | 41.5 MB | 251 MB | 8.3 MB |
| 8 MB downloads, one at a time | **8.6 GB/s** | 7.7 GB/s | 2.1 GB/s | 1.7 GB/s |
| HTTPS, 64 at once | **162k req/s** | 116k | 28.3k | 128k\* |

\* Rust's multi-threaded runtime; its single-threaded one is in the README.

### npm packages

[Cap's media server](bench/cap-media/README.md) on Bun and, unchanged, on Barm (macOS, Barm's own JavaScriptCore, medians of 3 runs of 10 s each):

| | Bun 1.4 | **Barm** |
|---|---:|---:|
| startup (exec to first `/health`) | 120 ms | **105 ms** |
| idle memory | 50 MB | 51 MB |
| `/health` (64 connections) | **35.3k req/s** | 33.3k req/s |
| `/video/probe` (4 in flight) | **2,409/s** | 2,084/s |
| CPU per probe | 794 µs | **579 µs** |
| `/audio/extract` (an ffmpeg subprocess, streamed) | 25.5/s | 25.5/s |
| peak memory under load | 143 MB | **129 MB** |
| memory 3 s after the load | **64 MB** | 76 MB |
| failed requests | 0 | 0 |

Barm does the media work on about a quarter less CPU than Bun, but with 4 probes in flight Bun finishes more of them each second: its `fetch()` client runs on a thread of its own, and Barm's runs on the event loop. Barm's binary is 34 MB with its engine and takes 3.8 s to build.

Where Barm is behind: a million calls from Barm into a small npm function (zod's `safeParse`) take 195 ms, against 97 ms when Bun runs the same program in JavaScript, because every call crosses from native code into the engine ([bench/npm/README.md](bench/npm/README.md)).

## Development

```sh
cargo test                        # checker snapshots, fuzzing, native run tests, HTTP and npm tests
scripts/linux.sh                  # the npm tests in a Linux container (any command: scripts/linux.sh cargo test)
scripts/jsc/build.sh              # Barm's own JavaScriptCore (`scripts/jsc/build.sh linux` for Linux's)
python3 bench/run.py              # cross-language benchmarks
python3 bench/http/run.py         # HTTP server benchmark (bench/http/linux.sh for Linux)
scripts/sanitize.sh run file.barm # run under ASan/UBSan in a Linux container
```

## Project layout

- `crates/barm/`: the compiler and the npm bundler (Rust, no dependencies)
- `runtime/`: the C runtime linked into every program (the event loop, HTTP server and client), the JavaScriptCore bridge (`js.c`, `napi.c`) and Node's built-ins for npm packages (`node.c`, `node/`)
- `scripts/jsc/`: Barm's build of JavaScriptCore and its patches
- `vendor/boringssl/`, `vendor/brotli/`, `vendor/zstd/`: BoringSSL (Apache-2.0) for TLS, and the brotli (MIT) and zstd (BSD) codecs. They're compiled once, and only programs that use them link them (see each `BARM.md`).
- `examples/`: sample programs
- `tests/`: checker snapshots, run tests, fuzzing, and npm, HTTP and parity tests
- `bench/`: benchmarks against C, Rust, Node and Bun, and Cap's media server on Bun and Barm
- `site/`: the landing page, a Barm program (`cd site && barm dev`)
- `docs/`: the [language spec](docs/spec.md)

# Barm

A TypeScript-shaped language built for coding agents. It compiles to small, fast native binaries.

- **Looks like TypeScript.** If you (or your agent) can write TS, you can write Barm.
- **Native and fast.** Compiles to a single binary with no bundled runtime. It keeps pace with Rust and is far ahead of Node and Bun.
- **Quick feedback.** Builds take around 0.1 s. Error messages name the problem and the fix, and every error has a code you can look up with `barm explain`.

> **Status:** early (milestone 3). Command-line programs and web servers compile and run: records, unions, generics, classes, interfaces, checked errors, a Node-style standard library, and Bun's server API. Real async (concurrent I/O) is still to come.

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
cargo install --path crates/barm  # puts `barm` in ~/.cargo/bin

barm run examples/shapes          # build and run main()
barm --watch server.barm          # rebuild and restart when a source file changes
barm dev                          # run package.json's "dev" script, as `bun dev` does
barm test examples                # run every test(...)
barm build examples/shapes -o app # write a native binary
barm check src/                   # type-check only (add --json for tools)
barm explain T0001                # what an error code means
```

The command line works like Bun's: `barm <file>` runs a program and passes it the arguments after the file name, `barm <script>` runs a `package.json` script (with `pre`/`post` scripts, the `npm_*` environment and Bun's output), and `--watch` restarts the program when the entry file or anything it imports changes. `--hot` isn't supported yet.

## How it differs from TypeScript

Barm keeps TypeScript's syntax and drops the parts that make programs hard to reason about or slow to run:

- No `any`, `null`, `==` or implicit type conversions.
- `int` and `f64` are separate types. Integer overflow traps instead of silently losing precision.
- Arrays, records and maps are values. Assigning one makes a copy, and the compiler flags code where TypeScript would have shared the value instead.
- To modify a caller's value, a function declares an `inout` parameter and the caller passes `&x`.
- Unions of object types must be narrowed before you use their fields, and a `switch` must cover every case.
- Classes are reference-counted, not garbage-collected. If instances of a class could reference each other in a cycle, the compiler asks you to mark the back-reference `weak`, make it `readonly`, or declare a `cyclic class` (which adds cycle collection for that class only).

The full list is in [docs/spec.md](docs/spec.md).

## Performance

Median wall time and peak memory on an Apple M4 Max. Every language prints identical output.

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

Barm checks for integer overflow by default (fib above); Rust with overflow checks takes 592 ms on fib. `points` is bound by floating-point add latency in all three compiled languages.

Binaries are 34–35 KB (33 KB stripped, the same as C; Rust's are about 470 KB), and a build takes 0.04–0.07 s (C 0.04–0.05 s, Rust 0.08–0.16 s). Run the benchmarks yourself with `python3 bench/run.py`; the method is in [bench/README.md](bench/README.md).

### HTTP server

Barm speaks Bun's server API — `Bun.serve`, `routes` with typed `req.params`, `Request`, `Response`, `Headers`, `URL` — so a Bun server ports with only Barm's usual edits (a `try` on calls that can throw). `tests/parity` runs a Bun server under Bun and its port under Barm and checks that every response matches.

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

Requests per second on Linux (M4 Max, Docker), 128 keep-alive connections, JSON route — Barm running the Bun benchmark's own code:

| | Rust (axum) | Bun | **Barm** |
|---|---:|---:|---:|
| 1 core | 294k | 257k | **371k** |
| 4 cores | 1.30M\* | 958k | **1.61M** |
| p99 at a fixed 600k req/s, 4 cores | 1.91 ms\* | — | **1.30 ms** |
| memory, 4 cores (PSS) | 5.1 MB\* | 97 MB | **1.6 MB** |

\* Thread-per-core Rust; tokio's default multi-thread runtime reaches 720k. With pipelining, Barm serves 8.1M JSON requests/s on 4 cores (Rust 2.4M). Method, macOS numbers and caveats are in [bench/http/README.md](bench/http/README.md).

## Development

```sh
cargo test                        # checker snapshots, fuzzing, native run tests, HTTP protocol tests
python3 bench/run.py              # cross-language benchmarks
python3 bench/http/run.py         # HTTP server benchmark (bench/http/linux.sh for Linux)
scripts/sanitize.sh run file.barm # run under ASan/UBSan in a Linux container
```

## Project layout

- `crates/barm/`: the compiler (Rust, no dependencies)
- `runtime/`: the C runtime linked into every program
- `examples/`: sample programs
- `tests/`: checker snapshots, run tests and fuzzing
- `bench/`: benchmarks against C, Rust, Node and Bun
- `site/`: the landing page, served by a Barm program (`cd site && barm dev`)
- `docs/`: the [language spec](docs/spec.md)

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

Median wall time on an Apple M4 Max. Every language prints identical output.

| benchmark | Rust | Node | Bun | **Barm** |
|---|---:|---:|---:|---:|
| binary_trees | 800 ms | 478 ms | 398 ms | **316 ms** |
| class_trees | 806 ms | 486 ms | 382 ms | **286 ms** |
| dispatch (virtual calls) | 84 ms | 207 ms | 115 ms | **42 ms** |
| map_insert | 561 ms | 980 ms | 834 ms | **343 ms** |
| nbody | 246 ms | 577 ms | 533 ms | **217 ms** |
| sort | 77 ms | 1190 ms | 706 ms | **40 ms** |
| strings | 246 ms | 521 ms | 211 ms | **222 ms** |
| fib | 463 ms | 1521 ms | 966 ms | 611 ms\* |

\* Barm checks for integer overflow by default. Rust with overflow checks takes 607 ms, and `barm build --unchecked` takes 451 ms.

Binaries are about 50 KB (Rust's are about 470 KB), and a build takes 0.08–0.13 s. Run the benchmarks yourself with `python3 bench/run.py`; the method is in [bench/README.md](bench/README.md).

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
| 1 core | 299k | 259k | **386k** |
| 4 cores | 1.27M\* | 964k | **1.65M** |
| p99 at a fixed 600k req/s, 4 cores | 1.91 ms\* | — | **1.30 ms** |
| memory, 4 cores (PSS) | 5.0 MB\* | 98 MB | **2.5 MB** |

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

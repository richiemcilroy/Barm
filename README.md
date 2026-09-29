# Barm

A TypeScript-shaped language built for coding agents. It compiles to small, fast native binaries.

- **Looks like TypeScript.** If you (or your agent) can write TS, you can write Barm.
- **Native and fast.** Compiles to a single binary with no bundled runtime. It keeps pace with Rust and is far ahead of Node and Bun.
- **Quick feedback.** Builds take around 0.1 s. Error messages name the problem and the fix, and every error has a code you can look up with `barm explain`.

> **Status:** early (milestone 1). Command-line programs compile and run. Classes, error handling, async and a standard library are still to come.

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
cargo build --release
export PATH="$PWD/target/release:$PATH"

barm run examples/shapes          # build and run main()
barm test examples                # run every test(...)
barm build examples/shapes -o app # write a native binary
barm check src/                   # type-check only (add --json for tools)
barm explain T0001                # what an error code means
```

## How it differs from TypeScript

Barm keeps TypeScript's syntax and drops the parts that make programs hard to reason about or slow to run:

- No `any`, `null`, `==` or implicit type conversions.
- `int` and `f64` are separate types. Integer overflow traps instead of silently losing precision.
- Arrays, records and maps are values. Assigning one makes a copy, and the compiler flags code where TypeScript would have shared the value instead.
- To modify a caller's value, a function declares an `inout` parameter and the caller passes `&x`.
- Unions of object types must be narrowed before you use their fields, and a `switch` must cover every case.

The full list is in [docs/spec.md](docs/spec.md).

## Performance

Median wall time on an Apple M4 Max. Every language prints identical output.

| benchmark | Rust | Node | Bun | **Barm** |
|---|---:|---:|---:|---:|
| binary_trees | 800 ms | 478 ms | 398 ms | **316 ms** |
| map_insert | 561 ms | 980 ms | 834 ms | **343 ms** |
| nbody | 246 ms | 577 ms | 533 ms | **217 ms** |
| sort | 77 ms | 1190 ms | 706 ms | **40 ms** |
| strings | 246 ms | 521 ms | 211 ms | **222 ms** |
| fib | 463 ms | 1521 ms | 966 ms | 611 ms\* |

\* Barm checks for integer overflow by default. Rust with overflow checks takes 607 ms, and `barm build --unchecked` takes 451 ms.

Binaries are about 50 KB (Rust's are about 470 KB), and a build takes 0.08–0.13 s. Run the benchmarks yourself with `python3 bench/run.py`; the method is in [bench/README.md](bench/README.md).

## Development

```sh
cargo test                        # checker snapshots, fuzzing, native run tests
python3 bench/run.py              # cross-language benchmarks
scripts/sanitize.sh run file.barm # run under ASan/UBSan in a Linux container
```

## Project layout

- `crates/barm/`: the compiler (Rust, no dependencies)
- `runtime/`: the C runtime linked into every program
- `examples/`: sample programs
- `tests/`: checker snapshots, run tests and fuzzing
- `bench/`: benchmarks against C, Rust, Node and Bun
- `docs/`: [spec](docs/spec.md), [design](docs/design.md) and [research](docs/research.md)

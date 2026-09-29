# Barm micro-benchmarks

Small programs written four ways (C, Rust, TypeScript, Barm) that implement the same algorithm on the same data and print identical stdout. The runner builds each one, times it, measures peak memory, and checks that the output matches C's.

Languages: `c`, `rust`, `node` and `bun` (both run `main.ts` directly), `scriptc` (Vercel Labs' TS→native compiler, run on the same `main.ts`), and `barm`.

## Benchmarks

| Benchmark | Size | What it exercises |
|---|---|---|
| `fib` | `fib(42)` | Recursive calls, integer add/compare. Barm `int` is 64-bit with overflow traps; JS uses f64 numbers (SMIs in the JIT). |
| `points` | 600M iterations | Small record churn: `v = add(v, one)` returns a fresh `{x, y}` f64 record every iteration. In C/Rust it's a struct in registers; in JS it depends on escape analysis; in Barm it's a value record. |
| `array_push` | 30M `f64` | Growable-array `push` (amortised growth), then a `for…of` sum and an indexed `xs[i]!` sum. |
| `map_insert` | 2M keys | String building (`"k" + i`), hashing, and a string → int map: insert all keys, then look every key up again and sum. |
| `binary_trees` | depth 18 | Benchmarks Game binary-trees (single-threaded): many short-lived recursive allocations plus one long-lived tree. Barm uses a recursive record `{ left: Tree \| undefined, right: Tree \| undefined }`; Rust `Option<Box<Tree>>`; C `malloc`/`free`; TS plain objects. |
| `nbody` | 10M steps | Benchmarks Game n-body (5 bodies): f64 arithmetic, `sqrt`, in-place mutation of records inside an array (`inout` in Barm). Prints energy before and after to 9 decimals. |
| `sort` | 3M `f64` | Park–Miller LCG (`state = state * 16807 % 2147483647`, exact in both 64-bit ints and JS doubles), then sort with a comparator. Prints min, max and the sum of every 1000th element. |
| `strings` | 3M strings | Template literals / formatting, `join(",")`, `split(",")`, `endsWith`, and byte lengths. |

Each `bench/micro/<name>/` holds `main.c`, `main.rs`, `main.ts`, `main.barm` and `expected.txt` (C's output, used as the reference when C isn't among the selected languages). Sizes are chosen so the C version takes roughly 0.2–1.5 s on an Apple M-series machine. The exception is `array_push`, where C takes about 55 ms. The work is memory-bound, and reaching 200 ms would need about 1 GB of array in C and about 3 GB of heap in Node/Bun.

## Running

```sh
cargo build --release                                  # for barm check / barm build
npm install --prefix bench/.tools scriptc              # optional: scriptc
python3 bench/run.py                                   # everything, 5 timed runs each
python3 bench/run.py --only fib,sort --langs c,barm    # a subset
python3 bench/run.py --runs 10 --warmup 1 --timeout 120
python3 bench/run.py --list
```

Python 3.9+ with only the standard library. Output is a markdown summary (median time, ratio against C), a detail table (median/min time, peak RSS, compile time, binary size unstripped and stripped, output check), and a list of anything not measured and why. Each run also writes `bench/results/<timestamp>.json` with every sample, the toolchain versions, and the machine's load average.

How each language is built and run:

| Lang | Build | Run |
|---|---|---|
| c | `cc -std=c11 -O2 main.c -lm` | the binary |
| rust | `rustc --edition 2021 -C opt-level=3 main.rs` (no Cargo, no deps) | the binary |
| rust-checked | as `rust`, plus `-C overflow-checks=on` (Barm's default integer semantics) | the binary |
| node | — | `node main.ts` (Node ≥ 23.6 strips types natively) |
| bun | — | `bun main.ts` |
| scriptc | `bench/.tools/node_modules/.bin/scriptc build main.ts -o … --no-keep-c`, with `SCRIPTC_NO_CACHE=1` so compile time is a clean build (`--scriptc-cache` to allow cache hits) | the binary |
| barm | `target/release/barm build main.barm -o …` | the binary |
| barm-unchecked | as `barm`, plus `--unchecked` (integer overflow wraps, like Rust release builds) | the binary |

Barm builds use a cache private to each benchmark session, so every program is really compiled. The runtime object is built once at the start of the session (outside the timing), the way it is on any machine after the first build, much like Rust's precompiled standard library.

A missing toolchain is reported as `skipped`. A failed build is `build failed` with the compiler's message kept in the JSON; for scriptc a rejected program is `unsupported` (the TS isn't rewritten to suit it). A run past `--timeout` (default 60 s) is `timeout`, a non-zero exit is `crashed`, and output that differs from C is `mismatch`. Binaries go to `bench/.build/`.

## How the versions correspond

- The TS and Barm files are nearly line-for-line identical. They differ only where the spec requires it:
  - Barm uses `int` or `f64` where TS says `number`.
  - `main.ts` ends with a `main()` call, because Barm's entry point is `function main()` and top-level calls aren't allowed.
  - Strings use `.byteLength` in Barm and `.length` in TS (the strings are ASCII, so the values are equal).
  - In `nbody`, Barm passes the bodies array with `inout`/`&` and binds it with `let`.
- Indexed access is written `xs[i]!` in both. TS accepts it, and it type-checks under `tsc --strict --noUncheckedIndexedAccess`.
- `nbody` mutates elements through the array (`bodies[i]!.vx -= …`) rather than through a local alias. Under Barm's value semantics, `const bi = bodies[i]!` is a copy, so mutating the alias wouldn't write back. The same code is kept in TS.
- `binary_trees` reads `t.left`/`t.right` into locals before the `=== undefined` test. See the checker issue below.
- Floats are printed with `toFixed(n)` in TS/Barm, `%.nf` in C and `{:.n}` in Rust. All four round from the exact binary value, and no printed value is a tie.

## Caveats (read before quoting numbers)

- **Noise.** Wall time is measured for the whole process. The published results were collected on a busy developer machine (load average 8–17 on 16 cores, including a browser and other builds), Within one invocation the five runs usually agree to within a few percent, but the same binary can be 2–3× slower in a session where many cores are busy, and slow machine-wide phases can skew one language's column relative to another's. The JSON records the load average. For quotable numbers, use an idle machine and `--runs 10`, and look at `min` as well as the median.
- **Startup.** Node and Bun times include runtime startup (~20–40 ms) and JIT warm-up, and their RSS includes the VM. That's deliberate: it's what a user of a CLI sees.
- **Library quality differs:**
  - `map_insert`: C uses a small hand-written open-addressing table (FNV-1a, cached hashes), Rust uses std `HashMap` with SipHash (DoS-resistant and slower to hash), and JS uses the engine's `Map`.
  - `sort`: C's `qsort` is unstable and calls the comparator through a function pointer. Rust's `sort_by` is a stable driftsort. JS uses TimSort.
  - `strings`: Rust's `split` borrows `&str` slices without allocating; C and JS allocate every piece.
- **Semantics differ in the corners.** Barm `int` arithmetic traps on overflow, while C and Rust release builds don't check. Rust bounds-checks indexing and C doesn't. Barm `xs[i]!` traps.
- **Memory management differs in `binary_trees`.** C frees explicitly, Rust drops, and JS/Bun rely on a generational GC, which suits this workload well. This is the single-threaded variant. The fastest Benchmarks Game entries use threads and arenas, and none is used here.
- **Binary size** is reported both as linked and after `strip`. Rust binaries carry std symbols unless stripped.
- **Apple clang contracts `a*b+c` into FMA by default** (`-ffp-contract=on`), unlike Rust and JS. The printed `nbody` energies still match to 9 decimals, but the C and JS computations are not bit-identical.

## scriptc notes (v0.1.7)

- `nbody` is **unsupported**: `error SC1090: compound assignment to fields of computed receivers are not supported yet` on `bodies[i]!.vx -= dx * mj` (9 sites).
- `sort` **times out**. With a comparator, scriptc's sort takes 0.6 s for 1M elements but runs for more than 60 s at 2M. At 3M it was still running after 9 minutes, so something is super-linear. The program is correct at smaller sizes: 30k, 100k, 300k and 1M elements all match Node.
- `array_push` compiles and is correct but is about 100× slower than C, consistent with the array and `for…of` costs described in `docs/research.md` §8. `points` (fresh object per iteration) and `binary_trees` are also 6–18× slower than C, because every object is `malloc`'d and reference-counted.

## Barm checker issues

Found while writing `main.barm` files against `docs/spec.md` with `barm check` at M0. The Barm files currently use the workarounds noted.

1. **No narrowing of property paths against `undefined`.** TypeScript narrows `t.left` after `t.left !== undefined`. The spec lists `x === undefined` / `x !== undefined` among the narrowing forms without saying whether `x` may be a property path. It does say "if code looks like TypeScript it behaves like TypeScript, or it is a compile error". The natural binary-trees `check`:

   ```ts
   function check(t: Tree): int {
     if (t.left === undefined || t.right === undefined) return 1
     return 1 + check(t.left) + check(t.right)
   }
   ```

   is rejected with:

   ```
   error[T0001] micro/binary_trees/main.barm:12:20: expected `Tree`, found `Tree | undefined`
      12 |   return 1 + check(t.left) + check(t.right)
     context: argument `t` of `check`
     why: the value may be `undefined`
     fix[placeholder]: provide a default: `?? <default>`
     fix[maybe]: assert it is defined (traps if not): `!`
   ```

   The same error appears for `t.right` (col 36), and for the simpler `if (t.left !== undefined) return 1 + check(t.left)`. The error has a code and fixes, so it's arguably spec-conforming. But it rejects idiomatic TS, and neither suggested fix is the clean rewrite: binding locals first (`const left = t.left`), which is what `main.barm` does. Either the spec should say that only local bindings narrow, or the checker should narrow `const`-rooted property paths. With value semantics nothing can mutate `t` between the test and the use, so the latter is sound.

2. **`inout` arguments must be `let` bindings (clarification, not a bug).** `const bodies = createBodies(); advance(&bodies, 0.01)` gives:

   ```
   error[V0001] micro/nbody/main.barm:127:19: can't reassign constant `bodies`
     127 |   offsetMomentum(&bodies)
     fix[safe]: declare `bodies` with `let`
   ```

   That's reasonable, since an `inout` callee may reassign the whole value. But spec §6 says `const` bindings "can be mutated in place", and its `addOne(&items)` example doesn't say `items` must be `let`. The spec should state the rule.

Everything else used here type-checks: recursive records, `Map<string, int>`, template literals, `join`/`split`/`endsWith`, `sort` with an `f64` comparator, `toFixed`, `1 << n`, `Math.max` on ints, field compound-assignment through `xs[i]!`, and top-level `const` with `Math.PI`.

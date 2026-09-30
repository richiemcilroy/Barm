# Barm — design proposal v0

Status: draft for discussion, 2026-09-29. Grounded in [research.md](research.md); section refs like (R4) point there.

## Thesis

> **TypeScript's surface, sound native semantics, and a compiler built as the agent's feedback loop.**

- Models already know TypeScript. Every Barm construct that *looks* like TS *behaves* like TS, or it is a compile error with a code and a fix. No silent divergence. (R1.3, R3)
- The things that aren't bound by training data — compile speed, diagnostic quality, navigation, soundness, runtime speed — are where Barm wins. (R1, R7)
- Not a TS-compat compiler: we don't compile npm or existing `.ts`. Every attempt at that drowns in coverage (R3). Barm is its own language with TS's shape, like ArkTS/AssemblyScript but sound and fast.

Name: *Barm*. File extension `.barm`.

## A taste

```ts
import * as fs from "std/fs"
import * as json from "std/json"

// Object types and arrays are VALUES (copy semantics, copy-on-write storage).
type Point = { x: f64, y: f64 }

// TS discriminated unions compile to native tagged unions.
type Shape =
  | { kind: "circle", center: Point, r: f64 }
  | { kind: "rect", min: Point, max: Point }

export function area(s: Shape): f64 {
  switch (s.kind) {                      // exhaustiveness is checked
    case "circle": return Math.PI * s.r * s.r
    case "rect":   return (s.max.x - s.min.x) * (s.max.y - s.min.y)
  }
}

// Errors are declared and propagated explicitly; no unwinding.
// Effects (`uses`) are part of an exported signature.
export function loadShapes(path: string): Shape[] throws fs.Error | json.Error uses fs {
  const text = try fs.readText(path)
  return try json.parse<Shape[]>(text)
}

// Mutation of a caller's value is explicit at both ends.
function addUnitCircle(inout shapes: Shape[]) {
  shapes.push({ kind: "circle", center: { x: 0, y: 0 }, r: 1 })
}
// caller: addUnitCircle(&shapes)

// Classes are REFERENCE types with identity (reference-counted).
class Scene {
  shapes: Shape[] = []
  weak parent: Scene | undefined         // strong cycles are a compile error
  total(): f64 { return this.shapes.map(area).sum() }
}

test("unit circle area", () => {
  expect(area({ kind: "circle", center: { x: 0, y: 0 }, r: 1 })).toBeCloseTo(Math.PI)
})
```

Rule of thumb an agent can hold in one line: **`class` is a shared reference; everything else is a value.**

## Language

### Types & numbers
- `int` (i64), `f64`, sized `i8…i64`, `u8…u64`, `f32`, `bool`, `string` (UTF-8, immutable value). `number` is an alias for `f64` so TS code reads naturally.
- Numeric literals take their type from *all* uses within the function (default `int`), so `let sum = 0; sum += 0.5` just works. Mixing `int`/`f64` otherwise requires explicit conversion — the diagnostic offers both fixes.
- Integer overflow traps at every tier; wrapping ops are explicit (`wrapping.add`).
- No `any`. `unknown` + narrowing. `as` is a *checked* cast.
- `T | undefined` for optionality, with TS narrowing (`if (x !== undefined)`, `?.`, `??`). `null` exists only for interop, or is dropped — **open**.
- Indexing returns `T | undefined` (TS `noUncheckedIndexedAccess`); `arr.at!(i)` traps on OOB.

### Object types, unions, interfaces
- `type X = { ... }` is an **exact record value** with a fixed layout. No width subtyping (extra fields are an error, as in scriptc). Two records with identical fields are the same type.
- Discriminated unions → tagged unions. Non-discriminated unions of distinct nominal types are allowed (tag is compiler-generated); ambiguous object-shape unions are a coded error with the rewrite.
- `interface` is **structural and implicit** (TS + Go): anything with the members satisfies it.
  - As a **generic bound** (`<T extends Printable>`) → static dispatch, instantiated.
  - As a **value type** (`x: Printable`) → fat pointer (data + method table), dynamic dispatch.
  - This keeps the STS-measured cost of structural access (R3) confined to where it's explicitly asked for.
- `class`: nominal reference type with fixed layout. **Single inheritance is supported** (`extends`, `super`, `abstract`, `implements`, getters/setters, statics, `#private`); no mixins or multiple inheritance. Agents write `extends` constantly (`class NotFound extends Error` alone justifies it), and it's cheap when done the scriptc way:
  - base fields are a prefix of the derived struct, so upcasts are free;
  - tier 2 knows the whole program, so methods nobody overrides are called directly; only actually-overridden methods get vtable slots (tier 0 uses vtables for everything, keeping incremental builds local);
  - `instanceof` is an O(1) range check on preorder class numbers.
  - Unlike scriptc (which rejects it with SC2002), a class instance **can** be used as a structural `interface` value — via the fat-pointer path above.

### Memory model (R5 — the biggest decision)
- **Values by default**: records, arrays, `Map`, `Set`, strings are values with copy-on-write storage. No aliasing → no lifetimes, trivially thread-safe, and the optimizer gets Rust-level no-alias facts.
- Parameters are borrowed-immutable by default; `inout` for mutation (call site `&x`) — mutating a non-`inout` param is a compile error with the fix. Values are destroyed at last use.
- **Divergence guard**: `const b = a; b.push(1); use(a)` would behave differently than TS. The compiler detects "copy mutated, then original read" and errors with an explanation. (This is the one place value semantics could surprise a TS-trained model; we make it loud.)
- Optionals and unions are stored **inline** as tagged values (`f64 | undefined` is 16 bytes on the stack), never heap boxes. Arrays of primitives/records are flat, indexed by `int`.
- `class` instances are **reference-counted**, non-atomic by default; `shared class` for cross-thread use (atomic RC). What makes this fast where scriptc's RC is slow (see [research.md §8](research.md#8-scriptc-internals-source-read--measured-v017--0d9d946-apple-silicon)):
  - **borrowed parameters by default**: callers don't retain arguments; only storing a reference retains (Swift/Lean convention; scriptc makes callees own every argument);
  - RC insertion and elision done **in Barm IR** (Perceus-style drop/reuse, ownership analysis), not left to the C compiler;
  - a small built-in **size-class allocator** (thread-local free lists, bump refill) instead of libc `malloc` per object;
  - the RC runtime is a few hundred bytes of code. A precise tracing GC would need stack maps or a shadow stack through the C backend — slower, larger, and harder to make incremental.
- **Static cycle check**: if the class graph permits a strong reference cycle, it's a compile error naming the path and offering both fixes — mark an edge `weak`, or declare `cyclic class`. Only `cyclic` types pay for cycle collection: an extra header and a synchronous trial-deletion collector (Bacon–Rajan, generational — scriptc's design, but opt-in and visible rather than inferred).
- `arena { }` blocks for bulk allocation; `unsafe` raw pointers for FFI.
- Validate in M2 against the scriptc/Node microbenchmarks (§ Benchmarks); a tracing GC remains the fallback if RC loses.

### Errors and effects
- `throws E` in the signature; call sites mark propagation with `try expr`. `try { } catch (e) { }` works as in TS with `e` typed as the union of thrown errors. Implemented as return values (zero-cost, no unwinding).
- Panics (`trap`) for bugs only: OOB `at!`, overflow, failed `as`. Not catchable.
- **Effects**: `uses fs | net | env | time | rand | proc`. Required on exported functions, inferred for private ones. Lets an agent (and a reviewer) see what a function can touch from its signature alone (Zero's capability idea, R2; tracked capabilities cost agents nothing measurable, R6). Capability-object passing (for mocking) vs annotations — **open**.

### Generics
- `function max<T extends Ord>(a: T, b: T): T`. Bodies checked once at definition (no C++/TS-conditional-type template errors).
- Instantiated by *layout shape*, deduplicated globally in the cache (R4). No conditional types, mapped types or type-level programming.

### Modules & grep-ability
- One file = one module. `import { a } from "./x"` and `import * as x from "std/x"`. No import renaming, no re-exports, no barrel files, no default exports (R6).
- No import cycles between packages (Go) — enables parallel, exact incremental builds.
- No macros. Built-in `derive`-like capabilities (json, eq, hash, debug, order) are compiler-provided. A restricted, hermetic `comptime` may come later.

### Concurrency
- `async`/`await` (TS familiarity) compiled to stackless state machines over a small std event loop (kqueue/epoll/IOCP) — no runtime unless you use it.
- `spawn` for OS-thread parallelism; values move freely across threads, `class` needs `shared class`.

### Dropped from TS (each is a coded error with a rewrite hint)
`any`, prototypes, `eval`, `with`, `arguments`, `Proxy`, `Symbol`, `==`/`!=` coercion (`==` means strict equality), dynamic property add/delete, index signatures (use `Map`), decorators, namespaces, enums (use unions of literals), conditional/mapped types, getter side effects, `string.length` (ambiguous under UTF-8 → `byteLength` / `chars().count()`).

## Toolchain — one binary, `barm`

| Command | Purpose |
|---|---|
| `barm check` | format + type-check + lint + affected tests → one pass/fail line, details on failure |
| `barm run` / `barm build [--debug]` | run instantly (tier 0, hot code tiers up) / ship a native binary (tier 2) |
| `barm test` | inline tests, snapshots (`--accept`), property tests, fuzz; test-impact selection |
| `barm fix [--to=edition]` | apply only `safe` fixes; edition migrations |
| `barm q sig\|refs\|callers\|impls\|api\|effects <symbol>` | semantic queries, paginated & ranked (also exposed via LSP + MCP) |
| `barm map [--budget 2k]` | ranked repo outline: signatures only, fitted to a token budget |
| `barm docs [topic]` | version-matched language/stdlib docs served by the compiler |
| `barm explain <CODE>` | long-form explanation + examples for a diagnostic |

**Diagnostics** (compact text default; `--json` for automation; byte-stable across runs so prompt caching works):

```
error[T0412] src/shapes.barm:14:3  switch on `s.kind` is not exhaustive
  missing: "triangle"  (Shape defined at src/shapes.barm:9)
  fix[safe]: add `case "triangle": …` stub

error[T0107] src/app.barm:22:17  `Shape` has no field `radius`
  did you mean: r   (fields: kind, center, r)
```

Every diagnostic: location, what was found, **the admissible alternatives** (R1.4), and a fix tagged `safe` / `maybe` / `placeholder`. No cascades — report root causes.

**Global content-addressed cache** (the 500GB fix, R4): `~/.cache/barm/`, keyed by `H(compiler, target, flags, input hashes)`, never paths. Per-function machine code, per-package interface hashes, instantiations. Worktrees hold only output binaries. LRU GC with a size cap. Optional remote cache later.

**Packages**: broad stdlib first (fs, net, http, json, crypto, cli args, time, process, testing). Registry later with namespaces, name-similarity blocking, default release cooldown, checksum DB (R6).

## Compiler architecture

Principles:
- **The fastest build is the one you don't do.** The inner loop never links, rarely codegens, and never re-checks what didn't change.
- **One pipeline, tiers — not dev/release modes.** One front end and one checked IR decide all semantics. Backends only choose how much to optimize a function, the way JS engines tier up hot code, but ahead of time. The only reason tiers exist: cross-function inlining (where most speed comes from) conflicts with per-function incrementality and debuggability.

```
edit
 │
 ▼  resident daemon (auto-started by the CLI, one per project)
parse → check ── query-based, early cutoff on unchanged interfaces
 │                └─► `barm check` / `barm q` answered from memory (~ms)
 ▼
Barm SSA IR ── RC/COW passes; per-function content hash keys everything
 │
 ├─ Tier 0  baseline   copy-and-patch, per function, µs; debuggable; runs immediately
 │                     (in a runner process fed by the daemon — no object files, no link)
 ├─ Tier 1  optimized  hot functions (counters in tier-0 code) recompiled in the background:
 │                     function + its callees → C → clang -O2 → loaded, slot swapped
 └─ Tier 2  ship       `barm build`: whole program → C → clang -O3 (+ThinLTO,
                       + PGO from tier-0 counters) → small static binary, no runtime
```

- **Compiler in Rust with zero dependencies** (fast rebuilds of the compiler itself, enums + exhaustive `match` for checker code, safety so the compiler never crashes on agent input). **Runtime, stencils and runner in C** — the code that ends up inside user programs. No LLVM linked into `barm`: the toolchain stays a small single binary and uses the system clang. Self-host in Barm once it can run real programs.
- **Front end**: data-oriented (flat arrays, u32 indices — Carbon/Zig style); per-file parallel parse; query-based semantic analysis (Salsa-style early cutoff). Exported signatures are fully annotated, so a body edit can't change a module's interface hash → dependents don't recheck.
- **Daemon**: the CLI connects to a per-project background process holding the checked program in memory (Sorbet/tsgo/rust-analyzer model), optionally watching files so results are ready before the agent asks. Cold path works without it.
- **Tier 0 — copy-and-patch** (Xu & Kjolstad, OOPSLA'21; CPython 3.13's JIT). Machine-code stencils per IR op are precompiled by clang when *`barm` itself* is built; compiling a function is memcpy + patching holes, reported as orders of magnitude faster than LLVM -O0 with comparable code. Calls go through a per-function slot table, so an edit replaces one slot. `barm run`/`barm test` execute in a separate runner process that maps code the daemon sends it (a crash in user code can't take down the daemon). Cranelift is the fallback if stencils prove too limiting.
- **Tier 1 — background optimization.** Hot functions are compiled with the callees worth inlining, via C → clang, and swapped into their slot. Each optimized unit records the hashes of everything it inlined; editing any of them drops it back to tier 0 instantly and re-optimizes in the background. Benchmarks and heavy tests get fast without a special build.
- **Tier 2 — ship.** `barm build` compiles everything at the top tier as whole-program C with clang extensions (`restrict` from value semantics, `__builtin_*_overflow`, `musttail`) → `clang -O3`, cached per module in the global CAS. Profiles collected at tier 0 feed PGO. Direct LLVM IR only if C can't express something we need; `zig cc` for cross-compiling. `barm build --debug` = tier-0-quality native binary for debuggers and GUI apps.
- **One semantics, enforced**: the language has no undefined behaviour and all tiers lower the same checked IR, so they must agree exactly. Every test runs at tier 0 and tier 2 and outputs must match (differential testing, as scriptc does against Node).

## Targets (measurable proxies — no paid agent evals needed)

| Proxy | Target |
|---|---|
| Language spec size | ≤ 8k tokens, plus an examples file |
| `barm check` after a one-file edit, 100k-LOC project (daemon warm) | < 10 ms |
| Cold check throughput | ≥ 1M lines/s |
| Edit → affected tests running (tier 0) | < 100 ms |
| `barm build` (tier 2), cached, one module changed | < 2 s |
| Tier-2 runtime vs equivalent Rust/C (benchmark suite) | within 10–20% |
| Hello-world binary | < 50 KB, no runtime |
| Disk for N worktrees of one commit | ≈ 1× |
| Tokens for equivalent programs vs TS / Go / Rust | ≤ TS (measured offline with tokenizers) |
| Diagnostic corpus: % with alternatives + safe fix | > 80% |

Agent evals come later, once there's something worth evaluating.

## Benchmarks

Every benchmark is compared four ways — **Barm vs Rust vs Node/Bun vs scriptc** — on clean and incremental compile time, binary size, runtime, peak memory, and source size (lines and tokens).

**Micro suite** (directly comparable with the scriptc numbers in research.md §8): `fib(32)`; 10M small-object `v = v.add(one)`; 5M `push` into `number[]`, then `for…of` and indexed iteration; `Float64Array` loop; 1M string-keyed `Map` inserts; binary-trees; JSON parse+serialize.

**Cap suite** (real code from the Cap monorepo, all CLI/server-shaped, no GPU/AV deps):

| # | Component | Source | Measures |
|---|---|---|---|
| 1 | Muxer wire protocol | `crates/cap-muxer-protocol` (Rust, ~750 LOC) | encode/decode MB/s, CRC32, bytes & integers |
| 2 | Transcript → WebVTT pipeline | `apps/web/lib/edit-transcript.ts` + `transcribe-utils.ts` (TS, ~750 LOC) | strings/JSON on large transcripts; same source vs Node/Bun/scriptc |
| 3 | Cursor/keyboard/caption timeline math | `crates/project/src/{cursor,keyboard,caption_timing}.rs` (Rust, ~2.8k LOC) | structs, maps, floats; real `cursor.json` fixtures |
| 4 | Recording meta/config loader | `crates/project/src/meta.rs` (+ slice of `configuration.rs`) | JSON parse/validate/migrate, cold start |
| 5 | `cap` CLI REST slice | `apps/cli/src/{caps,library}.rs` vs a mock API | startup time, binary size vs Rust |
| 6 | Discord webhook server | `apps/discord-bot/src/index.ts` (TS, ~400 LOC) | req/s, p99, memory, cold start; ed25519 + JWT (needs M6) |

Fixtures: `cap-performance-fixtures/reference-recording.cap` (meta/config JSON, logs, `cursor.json`, MP4s).

## Roadmap

- **M0 — spec + core front end. ✅ Done (2026-09-29).** Spec (~1.8k tokens); lexer/parser/checker for the core (numbers, strings, records, unions, narrowing, functions, closures, generics, interfaces, modules, tests); `barm check`/`--json`/`explain`; 127 diagnostic codes with alternatives and fixes; UI snapshot tests; mangled-input robustness test. Cold check ≈ 1.1M lines/s.
- **M1 — it runs. ✅ Done (2026-09-30).** Tier 2 C backend (records → structs, unions → tagged unions, recursive aliases → refcounted boxes, generics monomorphized, closures with stack or heap environments, copy-on-write arrays/maps/sets/strings); C runtime with Node-exact number formatting and `console.log` layout; `barm build/run/test` with a content-addressed binary cache; micro-benchmark suite vs C/Rust/Node/Bun/scriptc (Barm 0.6–1.4× C); differential tests vs Node; sanitizer audit in a Linux container. Also landed: field-path narrowing, the copy-then-mutate check (V0120).
  Known gaps: interfaces as value types, `unknown` at runtime, closures returning into mismatched function types, `console.log("%s", …)` format specifiers, per-module (rather than whole-binary) caching.
- **M1.5 — instant loop.** Daemon + tier 0 (copy-and-patch) + runner process; differential tests vs tier 2. Benchmarks: edit→test latency.
- **M1.75 — tier-up.** Hotness counters, background tier 1, slot swapping and inlining-aware invalidation.
- **M2 — memory model validation. ✅ Done (2026-09-30).** Classes (single inheritance, `super`, `abstract`, `implements`, getters, statics, visibility, `readonly`, parameter properties, generic classes); reference counting with borrowed parameters, moved constructor arguments and direct (inlinable) drops; the static cycle check (`weak` back-references, `readonly` set-once references, or `cyclic class` with a trial-deletion cycle collector that only cyclic classes pay for); interface values as fat pointers; `C | undefined` as a nullable pointer; devirtualized method calls with `switch` dispatch for overridden methods. Also fixed: narrowing soundness across nested blocks, loops and closures; unbounded-depth frees (programs run on a 1 GiB reserved stack). Benchmarks: class-based binary trees 2.8× faster than Rust, virtual dispatch 2× faster than Rust and C.
  Known gaps: setters, generic methods, closures capturing `this` stored on the object aren't traced by the cycle collector, `instanceof` on interface values.
- **M3 — real programs. ✅ Errors and std done (2026-09-30).** `throw`/`try`/`catch`/`finally`, `throws` (declared or inferred), `try f()` propagation markers, built-in `Error` classes (written in Barm, in a prelude module); errors are return values checked by one branch after calls that can throw. Standard library with Node's names (`node:fs`, `node:path`, `process`, `Date.now`, `performance.now`), written in Barm over a small `__native` runtime layer; `JSON.stringify` and a typed, validating `JSON.parse` generated per type (faster than Bun: stringify+parse of 200k records 238 ms vs 273 ms). Rest parameters. A small-string allocator in the runtime.
  Still to do: effects (`uses`), closures that throw, C FFI.
- **M4 — fast.** Optimization passes in Barm IR (inlining, escape analysis, RC elision, bounds-check elimination), PGO, benchmark suite vs Rust/C/Go.
- **M5 — agent toolchain.** `q`, `map`, `fix`, `docs`, LSP + MCP.
- **M6 — async, net/http, crypto, packages.** Port Cap #6.

A minimal HTTP server moves forward to right after M3, so the headline requests/sec benchmark can run early. **✅ Done (2026-09-30):** `std/http` — a Bun-style `serve({ fetch })` written in Barm over a native event loop (kqueue/epoll, level-triggered, one `read` and one `write` per request, pipelining with output backpressure) and forked workers that share the socket with even connection counts. On Linux it serves 1.5× the requests/s of Rust (axum/tokio) on one core and 1.3–2.5× on four, at 3–9 MB RSS; details in bench/http/README.md.

## Open decisions

Decided (2026-09-29):
- **First target: CLIs and servers.** Desktop/media later.
- **`class` memory: RC with borrowed params, IR-level elision, own allocator, static cycle check + opt-in `cyclic class`.** Confirm with M2 benchmarks.
- **Single inheritance: yes**, scriptc-style layout and devirtualization, plus structural interfaces for classes.

Still open:
1. **Effects**: signature annotations (recommended to start) vs capability objects.
2. **`null`**: drop in favour of `undefined` only, or keep both as in TS.

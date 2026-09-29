# Barm research notes

Compiled 2026-09-29 from five parallel research passes (Vercel Labs projects; agent-first language discourse; TS→native compilers; fast compilers & memory models; agent toolchain ergonomics). Figures are as reported by the linked sources — spot-check before quoting publicly.

Legend: **[E]** empirical, **[V]** vendor/self-reported, **[O]** opinion/anecdote.

---

## 1. The headline findings

1. **Cost is driven by turns and retries, not by syntax length.** Agentic tasks run ~154:1 input:output tokens; failed runs use 33–440% more input tokens than successful ones. Compressing tool output by 38% *raised* cost 6.8% because agents re-fetched missing detail. **[E]** ([arXiv 2604.22750](https://arxiv.org/abs/2604.22750), [2511.11012](https://arxiv.org/abs/2511.11012), [2607.12161](https://arxiv.org/abs/2607.12161), [JetBrains on rtk](https://blog.jetbrains.com/ai/2026/07/rtk-claude-code-token-savings/))
2. **Terse syntax doesn't win.** J/APL-style density did not win in Dan Luu's 15-language study; OCaml cost 1.3–1.7× more tokens because agents looped on type errors, not because source was longer. **[E]** ([danluu.com/pl-tokens](https://danluu.com/pl-tokens/), [Tokenmaxxing 2607.22807](https://arxiv.org/html/2607.22807v1))
3. **Training data matters, but less than feared for frontier agents.** Zero-data languages start at 0–1% zero-shot (MoonBit, Gleam); a zero-data language with spec-in-context trailed Python by 27–39 pts (PyLang). But well-constrained designs reach near parity on small tasks (VeraBench ~98.7% vs TS 99.7%), and MirrorCode found little solve-rate difference across Python/C/Rust/Go/OCaml/Ada. **[E]** ([2606.16827](https://arxiv.org/html/2606.16827v1), [2605.15607](https://arxiv.org/html/2605.15607v2), [vera-bench](https://github.com/aallan/vera-bench), [2606.30182](https://arxiv.org/html/2606.30182))
   → **Look like the training distribution.** Most failures in new languages are "right algorithm, wrong syntax/semantics".
4. **94% of LLM compile errors are type errors**, and diagnostics that list *admissible alternatives* raised repair success by +42–44pp; format (prose vs JSON) barely mattered. **[E]** ([PLDI'25 2504.09246](https://arxiv.org/pdf/2504.09246), [VeriHarness 2607.14167](https://arxiv.org/abs/2607.14167))
5. **Localization is ~half the budget; context overflow and "endless file reading" are top failure modes.** Symbol-level navigation cut total tokens 23% and raised resolve rate ~6pp. **[E]** ([SHERLOC 2606.24820](https://arxiv.org/abs/2606.24820), [SWE-Bench Pro 2509.16941](https://arxiv.org/pdf/2509.16941))
6. **Type checking slows small-task loops 1.6–3.2×** (mypy/Steep) — so the checker must be *fast*, or the correctness win is eaten by latency. **[E]** ([mame/ai-coding-lang-bench](https://github.com/mame/ai-coding-lang-bench))

## 2. What Vercel Labs shipped

### scriptc (July 2026) — "ordinary TypeScript → native, no JS engine"
- Real `tsc` front end → typed IR (monomorphized generics, tagged unions, explicit closure captures) → C / LLVM IR / native / WASI. Per-feature precompiled runtime. **[V]** ([repo](https://github.com/vercel-labs/scriptc), [how it works](https://scriptc.dev/how-it-works))
- Every construct is *static*, *dynamic island* (embedded quickjs-ng, `--dynamic`), or *rejected* with an `SCxxxx` code + rewrite hint. `scriptc coverage` reports % static.
- RC + deterministic cycle collector; stackful fibers for async; UTF-8 strings; exact record shapes (extra fields = error); `JSON.parse` returns `unknown` with checked casts.
- Numbers: 1.78ms startup vs Node 61.78ms, ~200–375KB binaries, 1.9MiB idle HTTP server. But Hono 18.4k req/s vs Bun 70.5k; independent tests 7.5–10× *slower* than Node on numeric work (everything is `f64`); ICEs on real projects. **[V]/[E]** ([InfoQ](https://www.infoq.com/news/2026/09/vercel-scriptc-node/), [HN](https://news.ycombinator.com/item?id=49063175), [dev.to](https://dev.to/remojansen/i-tried-to-compile-typescript-into-a-native-binary-with-scriptc-5d07))
- Criticism (Filip Pizlo): floats-only skips "half the problem of fast JS"; `any` is common enough that real programs fall into the slow island. **[O]**

### Zero / zerolang (May 2026) — "the programming language for agents"
- New Rust/Zig-like syntax; explicit ownership, allocators, `raises`/`check`; capability-passing (`World`) so side effects are visible in signatures; own ELF/Mach-O/COFF emitters (~1ms builds, ~16KiB hello). **[V]** ([repo](https://github.com/vercel-labs/zerolang), [zerolang.ai](https://zerolang.ai))
- Agent features: stable diagnostic codes with rule/expected/actual/fix-kind; `zero explain`; `zero fix --plan --json`; compiler-served, version-matched docs (`zero skills get`, ~6KB language / ~39KB stdlib); since v0.3 a binary semantic graph is the source of truth, text is a projection, edits are hash-guarded patches.
- **No published agent results.** Low HN traction; core objection: no training data. Frequent breaking changes; mostly quiet since late June. **[O]**

### Takeaways
- **Adopt:** coded diagnostics with fix hints; compiler-served versioned docs; capabilities visible in signatures; explicit "compiles / needs opt-in / rejected" contract; differential testing vs a reference; inspectable IR.
- **Do better:** keep *text* as source of truth with TS-shaped syntax (Zero's biggest criticism); design semantics that don't need a JS engine (scriptc's biggest problems are JS semantics: f64-only, `any`, cycle collector); fast compiles (scriptc depends on `tsc`-on-Node + LLVM); actually publish evidence; don't ship agent-dumped code with inflated READMEs.

## 3. TypeScript → native: lessons from ~20 attempts

Projects: Static Hermes, scriptc, Perry, ChadScript, Porffor, AssemblyScript, Static TypeScript (MakeCode), ArkTS, Tsonic, TypeScriptCompiler (MLIR), ts-llvm/StaticScript/Nerd (stalled), Hopc, weval, Wasmnizer-ts, Jawsm, Javy; plus Bun/Deno `--compile` (57–75MB bundled-runtime binaries) and tsgo/TS 7 (compiler in Go, 8–12× faster checking).

**"Compile existing TS" always drowns in coverage**: scriptc "hundreds of errors" on real projects; Static Hermes typed TS still "experimental and incomplete" after ~3 years; Porffor ~68% Test262. Successful production variants (ArkTS, STS, AssemblyScript) all became *their own language with TS syntax*.

**TS features incompatible with fast sound native code, and the known fixes:**

| Problem | Fix |
|---|---|
| `number` is always f64 | first-class `int` (i64) and `f64`; integer literals infer `int` |
| Structural typing of object values (STS: interface field access +10–102%) | nominal/fixed-layout concrete types; structural `interface` only at conversion to an interface value (Go model) |
| Unsoundness: `any`, unchecked `as`, covariant arrays | no `any`; `unknown` + checked narrowing; checked `as`; invariant mutable arrays |
| Dynamic property add/delete, index signatures | fixed layouts; explicit `Map<K,V>` |
| Untagged unions of shapes (scriptc ICE source) | discriminated unions compiled to tagged unions; exhaustive `switch` |
| Implicit `undefined` from OOB reads | `arr[i]` is `T \| undefined` (noUncheckedIndexedAccess semantics) |
| prototypes, `eval`, `with`, `arguments`, `Proxy`, `Symbol`, `==` | drop |
| closures, async, generators | keep, but design in from day one (AssemblyScript never got closures; generators were Porffor's hardest part) |

**Rule: never silently diverge from TS.** If it looks like TS it behaves like TS, or it's a compile error with a code.

## 4. Fast compiles *and* fast code

**Why Rust is slow**: LLVM dominates binary crates; linker dominates incremental debug; monomorphization compiles code nobody calls (`-Zhint-mostly-unused` halved one release build); proc macros tripled code size in one project; frontend is serial. 55% of Rust devs wait >10s per rebuild. ([Rust survey 2025](https://blog.rust-lang.org/2025/09/10/rust-compiler-performance-survey-2025-results/), [kobzol](https://kobzol.github.io/rust/rustc/2024/03/15/rustc-what-takes-so-long.html), [nnethercote](https://nnethercote.github.io/2025/06/26/how-much-code-does-that-proc-macro-generate.html), [Rust 2026 fast-builds roadmap](https://goals.rust-lang.org/2026/roadmap-fast-builds.html))

**What fast compilers do** (common pattern): fast non-optimizing dev backend + LLVM only for release; language rules that make dependencies exact (Go: no import cycles, export data up front); data-oriented compiler internals (Carbon target: 10M lines/s lex, 1M lines/s check); compiler owns dev linking and patches binaries in place (Zig, Roc surgical linker).
- Zig self-hosted x86 backend: hello 918→275ms, compiler build 75→20s; incremental in-place patching. ([ziglang devlog](https://ziglang.org/devlog/2025/))
- TPDE: LLVM-IR backend 8–26× faster than LLVM -O0 at on-par runtime, <8k LOC. ([2505.22610](https://arxiv.org/pdf/2505.22610))
- Cranelift for Rust: ~20% less wall / 40% less CPU vs LLVM debug. QBE: 25–75% of LLVM runtime speed.
- Linkers (Chromium debug): lld 16.6s, wild 4.0s, mold 1.65s.

**Caching / the 500GB problem**: Unison (definitions keyed by content hash), Salsa (early cutoff + durability), Go/Zig global per-user content-keyed caches. Cargo is only now adding a cross-workspace shared cache (2026 goal). Design: key every artifact by `H(compiler id, target, flags, input hashes)`, never by path; one per-user store; remap paths in debug info; LRU GC under a size cap. Five worktrees of the same commit ≈ one copy.

**Generics**: full monomorphization = best runtime, worst compile; dictionaries = the reverse; Go GC-shape stenciling in between (but blocks inlining). Hybrid: check once at definition (Carbon), instantiate at use site, dedupe by layout shape globally.

## 5. Memory model — the contested decision

Two research passes disagreed:

- **Value semantics + compiler-optimized RC** (Hylo/Swift + Perceus/Lobster/Lean): no lifetimes to annotate; Rust-level no-alias guarantees for the optimizer; deterministic, no pauses; Perceus RB-tree within 10% of C++ `std::map`; Lobster removes ~95% of RC ops at compile time; Vale: RC +25% vs generational refs +11% on one benchmark. ([Hylo paper](https://arxiv.org/pdf/2106.12678), [Perceus](https://www.microsoft.com/en-us/research/uploads/prod/2021/06/perceus-pldi21.pdf), [Lobster](https://aardappel.github.io/lobster/memory_management.html), [Vale](https://verdagon.dev/blog/generational-references))
- **Small precise tracing GC** (AssemblyScript, ChadScript, Go): STS measured *naive* RC at +64–165% vs mark-sweep and replaced it; AssemblyScript dropped RC for a ~2KB tracing GC; RC without cycle handling leaks, and agents will create cycles. ([STS paper](https://mmoskal.github.io/pdf/static-typescript.pdf), [AS v0.18](https://github.com/AssemblyScript/assemblyscript/releases/tag/v0.18.0))

**Reconciliation**: the anti-RC evidence is about *naive* RC applied to *every* object (STS, AS). The pro-RC evidence is about RC applied only where sharing exists, with compile-time elision. If most data are values (no RC at all), RC only touches `class` instances, and a static check forces cycles to be explicit, the STS failure mode doesn't apply. This is the first thing to prototype and microbenchmark.

## 6. Agent toolchain — what the evidence supports

- **Diagnostics**: location + observed + *admissible alternatives* (+42–44pp); rustc-style applicability levels (`MachineApplicable` etc.) with a `fix` command applying only safe ones; compact text by default (Zero: "JSON is for automation").
- **One check command** that formats, type-checks, lints, and runs affected tests with a single pass/fail (Claude Code best practices; SWE-agent linter-gated edits +3pp; Ronacher: TS running despite type errors confuses agents).
- **Semantic query API** (`sig`, `refs`, `callers`, `impls`, `package-api`, `effects-of`), paginated and ranked — gopls MCP and Claude Code's LSP tool are the closest precedents.
- **Repo map on demand** (Aider: PageRank over def/ref graph in ~1k tokens).
- **Grep-ability**: package-qualified names, no re-exports/barrel files/import aliasing, no macros (Ronacher). Claude Code uses grep, not vector search.
- **Version-matched docs shipped with the compiler.** Agents read instruction files (60.5% of doc interactions) far more than API references (1.3%); AGENTS.md associated with 28.6% lower runtime. llms.txt: no measurable effect. ([2608.20195](https://arxiv.org/abs/2608.20195), [2601.20404](https://arxiv.org/abs/2601.20404))
- **Canonical formatter**: TokDrift — semantics-preserving whitespace/casing changes flip model outputs 6–9% of the time. ([2510.14972](https://arxiv.org/html/2510.14972))
- **Keep meaningful names**: identifier obfuscation dropped HumanEval-X pass@1 76.5→40.2 (argues against Vera-style nameless slots).
- **Stability**: LLMs use deprecated APIs 25–38% of the time; Go rebuilt `go fix` with modernizers because assistants write stale idioms; Zig churn hurts agents. → compatibility promise + editions + `barm fix --to=<edition>`.
- **Packages**: 2026 frontier models hallucinate package names 4.6–6.1% of the time; 127 names hallucinated by all five models tested. → broad stdlib, registry with namespaces, similarity blocking, default release cooldowns.
- **Testing**: inline `test` blocks, snapshot tests, property tests (agentic PBT: ~56% of 984 reports were real bugs), fuzzing, test-impact selection from the build graph. Optional contracts: vericoding success Dafny 82% vs Verus 44% vs Lean 27% — automation beats expressiveness.

## 7. Strongest arguments against building Barm (keep these honest)

1. Training data dominates; every model starts at zero on Barm and keeps improving on Go/Rust/TS.
2. Go already has most agent-friendly properties (fast builds, gofmt, explicit errors, big stdlib). Nobody has shown a measurable win over Go.
3. None of the ~42 catalogued agent-first languages ([agentlanguages.dev](https://agentlanguages.dev/)) has controlled multi-run evidence.
4. Every release is stale in model weights; the language must stay stable or ship corrective docs forever.
5. Ecosystem, debuggers, human reviewers still matter; machine-friendly features can repel humans (Lattner: "A new programming language for LLMs doesn't make sense").

**Implication for Barm**: win on the things that *aren't* training-data-bound — the feedback loop (compile speed, diagnostic quality, navigation), soundness, and performance — while borrowing TypeScript's surface so models' existing knowledge transfers.

## 8. scriptc internals (source read + measured, v0.1.7 @ `0d9d946`, Apple silicon)

**Pipeline**: TypeScript 7 (native `tsgo`) type-checks over RPC → lowering → typed IR (f64-only, monomorphized generics) → textual LLVM IR → bundled out-of-process LLVM 22 at O2 → system linker + precompiled per-feature runtime objects. Content-addressed cache of whole executables; **no per-module incrementality**.

**Measured compile**: hello 0.34 s, small benchmark 0.53 s, HTTP server 0.49 s; cached rebuild 0.35–0.48 s.

**Measured binary size** (docs' ~320 KB claim is stale): hello 70.6 KB (68 KB stripped), with a regex 145 KB, `node:http` server 229 KB. Runtime is ~107k lines of C; features (regex, zlib, TLS, quickjs island) linked only when used, `-dead_strip`/`--gc-sections`, no LTO.

**Classes**: full support (`extends`, `super`, abstract, `implements`, accessors, statics, `#private`). Standalone class = `{rc; fields}`; in a hierarchy `{rc; vtable; base fields…; derived fields…}`. Whole-program devirtualization: vtable slots only for methods actually overridden. `instanceof` = preorder-interval check. **But** class instances can't be passed as interfaces with methods (SC2002), and width-subtyped object passing *copies* (mutations silently don't propagate — diverges from Node).

**Memory**: non-atomic RC (`size_t rc` first field; immortal sentinel checked on every op). Callees own parameters → caller retains every argument; no borrow inference or elision pass — relies on LLVM O2, which can't see into the precompiled runtime (no LTO). libc `calloc`/`free` per object. Closures box every captured variable on the heap. `T | undefined` and other unions are **heap-allocated boxes** — `for…of` over `P[]` allocates per element even after O2. Arrays: 8-byte slots + "present" bitmap + sparse side store; indices are doubles. Cycle collector: synchronous generational Bacon–Rajan trial deletion, only for types a whole-program analysis marks possibly-cyclic (+32-byte hidden header); runs at exit, event-loop idle, and thresholds.

**Measured runtime vs Node 26**:

| Workload | scriptc | Node |
|---|---|---|
| `fib(32)` | 8 ms | 16 ms |
| 10M small-object allocs (`v = v.add(one)`) | 115 ms | 26 ms |
| 5M `number[]` push | 148 ms | 16 ms |
| `for…of` over it | 342 ms | 27 ms |
| indexed loop over it | 141 ms | 4 ms |
| `Float64Array` loop | 5 ms | 7 ms |
| 1M string-keyed Map inserts | 114 ms | 190 ms |

Peak memory 205 MB vs Node 315 MB. Root causes: malloc per object, non-inlinable runtime calls, f64 indices, boxed unions, owned-parameter RC.

**Steal**: coded diagnostics + rewrite hints + `coverage`; checked `JSON.parse` casts with failing path (`expected number at $.port`); prefix layout + override-only vtables + preorder `instanceof`; cycle headers only on cyclic types; exceptions as a pending flag checked after throwing calls (no setjmp); byte-for-byte differential tests + ASan RC audit; per-feature runtime link manifest.

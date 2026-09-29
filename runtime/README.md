# Barm C runtime

`barm.h` is the contract between generated C and the runtime; `barm.c` implements it
(C11, libc + libm only). Generated programs `#include` both into one translation unit,
so every internal helper is `static` and prefixed `bm_`. Link with `-lm`.

What it provides:

- **Traps and memory**: `bm_trap` flushes stdout, prints `trap: <msg>\n  at <loc>` to stderr
  and exits 101 (or fails the running test). `bm_alloc`/`bm_realloc` trap on out-of-memory.
- **Strings**: immutable, refcounted, NUL-terminated UTF-8 (`bm_str`). One-character ASCII
  strings and `true`/`false` are immortal and never allocated. JS methods with byte indices
  (slicing through a character traps), JS `String(number)` (shortest round-trip digits),
  `toFixed`, `parseFloat`/`parseInt`.
- **Arrays**: copy-on-write, geometric growth, element types described by `bm_type`
  (retain/release/eq/hash/to_str/inspect). Stable merge sort, JS slice/concat/join semantics.
- **Maps/Sets**: insertion-ordered hash maps (entry array plus a linear-probing index,
  backward-shift deletion, and compaction of deleted entries). Copy-on-write. Sets are maps
  whose value type has size 0. Map keys use SameValueZero (so `NaN` matches `NaN`).
- **Console**: a port of Node's `util.inspect` layout with console.log's defaults (breakLength
  80, compact 3, depth 2): single-line vs multi-line containers, column-grouped arrays, `[Array]`
  beyond depth 2, "... n more items" beyond 100, Node's string quoting. Records format through
  `bm_inspect_record`. Stdout is buffered (64 KiB) and flushed at exit, before traps and before
  stderr writes (per line when stdout is a TTY).
- **Math, closures, tests**: `Math.round`, checked float→int, seeded `Math.random`
  (fixed seed; `BARM_SEED=<n>` picks a seed, any other value seeds from the clock),
  closure environments, and the `test(...)` runner.

## Tests

```sh
runtime/test.sh            # -Werror compile check, -O2 run, ASan+UBSan run, trap exit status
runtime/test.sh --docker   # same inside gcc:14 (adds LeakSanitizer; use where ASan doesn't run)
```

`test_runtime.c` is a standalone program: it includes `barm.c` directly and checks element
ownership with a counting `bm_type` whose retain/release calls are tallied per object.
`test_runtime trap` exits through a trap so the script can check the exit status.

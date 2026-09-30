/* barm.h — the runtime interface used by C code generated from Barm.
 *
 * Conventions
 * - Single-threaded; reference counts are non-atomic.
 * - Heap values (strings, arrays, maps, boxes, closure environments) start with an
 *   `int64_t rc`. rc < 0 means immortal (static literals): retain/release ignore it.
 * - Values are *owned* (+1) or *borrowed* (+0). Unless a comment says otherwise:
 *   arguments are borrowed, return values are owned, and `void *elem`/`key`/`value`
 *   inputs to container "insert" operations are MOVED (the container takes ownership).
 * - Containers are copy-on-write: mutating operations call *_make_unique first, which
 *   clones the buffer when rc > 1 (retaining the elements it copies).
 * - Element/key/value types are described by `bm_type` descriptors emitted by the compiler.
 * - Out-of-range indices, integer overflow, failed `!` assertions etc. call bm_trap(),
 *   which prints "trap: <msg>\n  at <loc>" to stderr and exits with status 101 —
 *   or, while a test is running, fails that test and continues with the next one.
 */
#ifndef BARM_H
#define BARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int64_t bm_int;

/* ------------------------------------------------------------------ traps */

_Noreturn void bm_trap(const char *msg, const char *loc);
void *bm_alloc(size_t size);          /* traps on out-of-memory */
void *bm_realloc(void *p, size_t size);
void bm_free(void *p);

/* ------------------------------------------------------------------ string builder */

/* Zero-initialize ({0}) before use. `data` is owned by the runtime (it sits inside a
 * larger allocation so bm_str_from_sb can adopt it without copying): read it freely, but
 * release it only with bm_sb_free / bm_str_from_sb / bm_out_sb_line, never free(). */
typedef struct bm_sb {
    char *data;
    size_t len, cap;
} bm_sb;

void bm_sb_push(bm_sb *sb, const char *s, size_t n);
void bm_sb_grow(bm_sb *sb, size_t need);          /* capacity >= need bytes */
/* Inline fast paths for generated code. */
static inline void bm_sb_add(bm_sb *sb, const char *s, size_t n) {
    if (__builtin_expect(sb->cap - sb->len < n, 0)) bm_sb_grow(sb, sb->len + n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
}
static inline void bm_sb_add_char(bm_sb *sb, char c) {
    if (__builtin_expect(sb->cap == sb->len, 0)) bm_sb_grow(sb, sb->len + 1);
    sb->data[sb->len++] = c;
}
void bm_sb_push_cstr(bm_sb *sb, const char *s);
void bm_sb_push_char(bm_sb *sb, char c);
void bm_sb_free(bm_sb *sb);

/* ------------------------------------------------------------------ type descriptors */

struct bm_type;
typedef struct bm_type {
    size_t size;                                  /* sizeof the C representation (0 allowed) */
    void (*retain)(void *p);                      /* NULL: plain data, nothing to retain */
    void (*release)(void *p);                     /* NULL: plain data, nothing to release */
    bool (*eq)(const void *a, const void *b);     /* value equality (===) */
    uint64_t (*hash)(const void *p);              /* consistent with eq */
    void (*to_str)(bm_sb *sb, const void *p);     /* JavaScript String(x) */
    void (*inspect)(bm_sb *sb, const void *p, int depth); /* console.log formatting (Node style) */
} bm_type;

/* console.log formatting of any value. The runtime's own descriptors leave `inspect` NULL (so a
 * program that never inspects doesn't link the formatter); this handles them. */
void bm_inspect_value(bm_sb *sb, const bm_type *t, const void *p, int depth);

/* Descriptors for primitives. */
extern const bm_type bm_type_int, bm_type_f64, bm_type_f32, bm_type_bool, bm_type_str, bm_type_undefined;
extern const bm_type bm_type_promise;   /* every Promise<T> (a bm_promise *): Node's `Promise { ... }` */
extern const bm_type bm_type_i8, bm_type_i16, bm_type_i32, bm_type_u8, bm_type_u16, bm_type_u32, bm_type_u64;

/* ------------------------------------------------------------------ strings (immutable UTF-8) */

typedef struct bm_strbuf {
    int32_t rc;        /* < 0: immortal (literals) */
    int32_t len;       /* bytes, excluding the trailing NUL (strings are < 2 GiB) */
    char data[];       /* NUL-terminated */
} bm_strbuf;

typedef struct bm_str {
    bm_strbuf *p;      /* never NULL; the empty string is bm_empty_str */
} bm_str;

extern const bm_strbuf bm_empty_strbuf;
#define BM_EMPTY_STR ((bm_str){(bm_strbuf *)&bm_empty_strbuf})

/* A static literal: BM_STR_LIT(name, "text") declares `name` usable as BM_LIT(name). Immortal
 * buffers are never written, so they're const (read-only pages, nothing in __data). */
#define BM_STR_LIT(name, text) \
    static const struct { int32_t rc; int32_t len; char data[sizeof(text)]; } name = { -1, sizeof(text) - 1, text }
#define BM_LIT(name) ((bm_str){(bm_strbuf *)&(name)})

static inline void bm_str_retain(bm_str s) { if (s.p->rc >= 0) s.p->rc++; }
void bm_str_release_slow(bm_str s);
static inline void bm_str_release(bm_str s) { if (s.p->rc > 0 && --s.p->rc == 0) bm_str_release_slow(s); }

bm_str bm_str_from(const char *bytes, size_t n);
bm_str bm_str_from_sb(bm_sb *sb);               /* takes the builder's contents; sb is reset */
bm_str bm_str_concat(bm_str a, bm_str b);
bool bm_str_eq(bm_str a, bm_str b);
int bm_str_cmp(bm_str a, bm_str b);             /* byte order == code point order */
uint64_t bm_str_hash(bm_str s);
static inline bm_int bm_str_byte_len(bm_str s) { return s.p->len; }

/* JavaScript-compatible conversions */
bm_str bm_str_from_int(bm_int v);
bm_str bm_str_from_f64(double v);               /* Number.prototype.toString(): 1 → "1", 0.1+0.2 → "0.30000000000000004", 1e21 → "1e+21" */
bm_str bm_str_from_bool(bool v);
bm_str bm_f64_to_fixed(double v, bm_int digits, const char *loc); /* JS toFixed (round half up on exact ties); traps if digits ∉ 0..100 */
bm_str bm_int_to_fixed(bm_int v, bm_int digits, const char *loc);
void bm_sb_push_int(bm_sb *sb, bm_int v);
void bm_sb_push_f64(bm_sb *sb, double v);       /* same format as bm_str_from_f64 */
void bm_sb_push_str(bm_sb *sb, bm_str s);

/* Parsing: returns false when the whole string (after trimming whitespace) isn't a number. */
bool bm_parse_float(bm_str s, double *out);     /* JS Number(s)/parseFloat subset: decimal, exponent, Infinity */
bool bm_parse_int(bm_str s, bm_int radix, bm_int *out);  /* radix 0 = 10 (or 16 with 0x); JS parseInt accepts a numeric prefix */

/* String methods (JavaScript semantics, with indices in BYTES; slice positions that split
 * a UTF-8 character trap). Negative indices count from the end, as in JS slice. */
struct bm_arr;
struct bm_arr bm_str_chars(bm_str s);           /* one string per Unicode scalar value */
bm_int bm_str_char_count(bm_str s);
bm_str bm_str_slice(bm_str s, bm_int start, bm_int end, bool has_end, const char *loc);
bool bm_str_includes(bm_str s, bm_str needle);
bool bm_str_starts_with(bm_str s, bm_str prefix);
bool bm_str_ends_with(bm_str s, bm_str suffix);
bm_int bm_str_index_of(bm_str s, bm_str needle);            /* byte index or -1 */
struct bm_arr bm_str_split(bm_str s, bm_str sep);           /* "" separator splits into characters */
bm_str bm_str_trim(bm_str s);
bm_str bm_str_trim_start(bm_str s);
bm_str bm_str_trim_end(bm_str s);
bm_str bm_str_to_upper(bm_str s);                           /* ASCII letters only in M1 */
bm_str bm_str_to_lower(bm_str s);
bm_str bm_str_replace(bm_str s, bm_str search, bm_str replacement);      /* first occurrence */
bm_str bm_str_replace_all(bm_str s, bm_str search, bm_str replacement);
bm_str bm_str_repeat(bm_str s, bm_int count, const char *loc);         /* traps on count < 0 */
bm_str bm_str_pad_start(bm_str s, bm_int len, bm_str fill);            /* len in characters */
bm_str bm_str_pad_end(bm_str s, bm_int len, bm_str fill);

/* ------------------------------------------------------------------ arrays (copy-on-write) */

/* Representation: the length lives in the value (so the C compiler can see it, e.g. a
 * constant length after inlining); the heap buffer holds rc, capacity and the elements.
 *
 * Sharing invariant: a buffer is shared (rc > 1) only through bm_arr_retain, i.e. by exact
 * copies of one value, and every mutation goes through bm_arr_make_unique (or a function that
 * calls it) before it changes elements or len. Hence all values sharing a buffer have the same
 * len, and the buffer owns exactly the elements [0, len) of any of them: bm_arr_release_slow
 * releases a.len elements. Elements at index >= len of a unique buffer are not owned (e.g.
 * after pop). A buffer with rc < 0 is immortal (a static literal) and is never mutated or
 * freed. Functions returning a bm_arr return either a fresh buffer (rc 1) or a retained copy
 * of an argument value (same len), which preserves the invariant. */
typedef struct bm_arrbuf {
    int64_t rc;
    int64_t cap;
    int64_t _pad[2];            /* keeps data 16-byte aligned (header stays 32 bytes) */
    unsigned char data[];
} bm_arrbuf;

typedef struct bm_arr {
    bm_arrbuf *p;               /* NULL = no allocation (len must be 0) */
    int64_t len;
} bm_arr;

#define BM_EMPTY_ARR ((bm_arr){NULL, 0})

static inline bm_int bm_arr_len(bm_arr a) { return a.len; }
static inline void *bm_arr_data(bm_arr a) { return a.p ? (void *)a.p->data : NULL; }
static inline void bm_arr_retain(bm_arr a) { if (a.p && a.p->rc >= 0) a.p->rc++; }
void bm_arr_release_slow(bm_arr a, const bm_type *t);
static inline void bm_arr_release(bm_arr a, const bm_type *t) { if (a.p && a.p->rc > 0 && --a.p->rc == 0) bm_arr_release_slow(a, t); }

/* Ensures *a is uniquely owned (rc == 1) with capacity >= min_cap (clones the value's len
 * elements, retaining them, when the buffer is shared or immortal). */
void bm_arr_make_unique(bm_arr *a, const bm_type *t, bm_int min_cap);
bm_arr bm_arr_with_capacity(const bm_type *t, bm_int cap);   /* len 0 */
/* Reserve capacity for exactly n more elements and return a pointer to where element a->len goes
   (buffer made unique; caller writes elements and then adds to a->len). */
void *bm_arr_reserve_tail(bm_arr *a, const bm_type *t, bm_int n);

/* Index checks: trap with "index <i> out of bounds for length <n>". */
_Noreturn void bm_arr_oob(bm_int i, bm_int len, const char *loc);
static inline void *bm_arr_at(bm_arr a, size_t elem_size, bm_int i, const char *loc) {
    if ((uint64_t)i >= (uint64_t)a.len) bm_arr_oob(i, a.len, loc);
    return a.p->data + (size_t)i * elem_size;
}
/* Mutable element access: makes the array unique first. */
void *bm_arr_at_mut(bm_arr *a, const bm_type *t, bm_int i, const char *loc);

void bm_arr_push(bm_arr *a, const bm_type *t, void *elem);         /* moves elem */
static inline void bm_arr_push_fast(bm_arr *a, const bm_type *t, void *elem) {
    if (a->p && a->p->rc == 1 && a->len < a->p->cap) {
        memcpy(a->p->data + (size_t)a->len * t->size, elem, t->size);
        a->len++;
    } else {
        bm_arr_push(a, t, elem);
    }
}
bool bm_arr_pop(bm_arr *a, const bm_type *t, void *out);          /* false if empty; out receives ownership */
bool bm_arr_shift(bm_arr *a, const bm_type *t, void *out);
void bm_arr_unshift(bm_arr *a, const bm_type *t, void *elem);      /* moves elem */
bm_arr bm_arr_slice(bm_arr a, const bm_type *t, bm_int start, bm_int end, bool has_start, bool has_end); /* JS semantics incl. negatives */
bm_arr bm_arr_concat(bm_arr a, bm_arr b, const bm_type *t);
void bm_arr_reverse(bm_arr *a, const bm_type *t);
/* Stable sort. cmp returns <0, 0, >0 like a JS comparator (a double). NULL cmp = JS default
 * (compare String(x) of each element). */
void bm_arr_sort(bm_arr *a, const bm_type *t, double (*cmp)(void *ctx, const void *x, const void *y), void *ctx);
bm_int bm_arr_index_of(bm_arr a, const bm_type *t, const void *elem);
bm_int bm_arr_last_index_of(bm_arr a, const bm_type *t, const void *elem);
bm_str bm_arr_join(bm_arr a, const bm_type *t, bm_str sep);       /* elements via t->to_str; "undefined" members print as "" (JS) */

/* ------------------------------------------------------------------ maps and sets (insertion-ordered, copy-on-write) */

typedef struct bm_mapbuf bm_mapbuf;   /* private layout */
typedef struct bm_map {
    bm_mapbuf *p;               /* NULL = empty map */
} bm_map;

#define BM_EMPTY_MAP ((bm_map){NULL})

void bm_map_retain(bm_map m);
void bm_map_release(bm_map m, const bm_type *kt, const bm_type *vt);
bm_int bm_map_size(bm_map m);
/* Borrowed pointer to the value for key, or NULL. Valid until the map is next mutated. */
void *bm_map_get(bm_map m, const bm_type *kt, const bm_type *vt, const void *key);
void bm_map_set(bm_map *m, const bm_type *kt, const bm_type *vt, void *key, void *value);   /* moves key and value; replaces (and releases) an existing value, keeps insertion position */
bool bm_map_has(bm_map m, const bm_type *kt, const bm_type *vt, const void *key);
bool bm_map_delete(bm_map *m, const bm_type *kt, const bm_type *vt, const void *key);
void bm_map_clear(bm_map *m, const bm_type *kt, const bm_type *vt);
bm_arr bm_map_keys(bm_map m, const bm_type *kt, const bm_type *vt);     /* insertion order, retained copies */
bm_arr bm_map_values(bm_map m, const bm_type *kt, const bm_type *vt);
/* Iteration in insertion order: for (i = 0; bm_map_next(m, kt, vt, &i, &k, &v);) — borrowed k/v. */
bool bm_map_next(bm_map m, const bm_type *kt, const bm_type *vt, bm_int *cursor, void **key, void **value);
/* Descriptor helpers for maps used as values inside other containers. */
bool bm_map_eq(bm_map a, bm_map b, const bm_type *kt, const bm_type *vt);   /* identity-free value equality */
void bm_map_inspect(bm_sb *sb, bm_map m, const bm_type *kt, const bm_type *vt, int depth); /* Map(2) { 'a' => 1, 'b' => 2 } */
/* Sets are maps whose value type has size 0: pass &bm_type_undefined as vt and NULL as value. */
void bm_set_inspect(bm_sb *sb, bm_map m, const bm_type *kt, int depth);                    /* Set(2) { 1, 2 } */

/* ------------------------------------------------------------------ console formatting */

/* Node's util.inspect as used by console.log (breakLength 80, compact 3, depth 2): numbers via
 * bm_sb_push_f64/int (-0 shown as -0), strings quoted inside containers (Node's quote choice
 * and escapes), arrays "[ 1, 2, 3 ]" ("[]" when empty), records "{ x: 1, y: 2 }" ("{}" when
 * empty), undefined, true/false. Containers switch to Node's multi-line layout (including
 * column-grouped arrays of more than 6 entries) when a line would exceed 80 columns, and print
 * [Array] / [Object] / [Map] / [Set] below depth 2. `depth` is 0 for a top-level console.log
 * argument; containers pass depth + 1 to their elements' inspect callbacks. */
void bm_inspect_str(bm_sb *sb, bm_str s, int depth);   /* depth 0 = top level: unquoted */
void bm_inspect_arr(bm_sb *sb, bm_arr a, const bm_type *t, int depth);
/* console.log formatting of a record: fields in declaration order. */
void bm_inspect_record(bm_sb *sb, int depth, size_t n, const char *const *names, const bm_type *const *types, const void *const *fields);
/* A class instance: `Name { a: 1 }` (`[Name]` past the depth limit). */
void bm_inspect_object(bm_sb *sb, int depth, const char *cls, size_t n, const char *const *names, const bm_type *const *types, const void *const *fields);
void bm_to_str_arr(bm_sb *sb, bm_arr a, const bm_type *t);   /* JS String([1,2]) == "1,2" */

/* ------------------------------------------------------------------ output */

/* Buffered stdout; flushed at exit and before traps. */
void bm_out_write(const char *s, size_t n);
void bm_out_sb_line(bm_sb *sb);          /* writes sb + "\n" to stdout, then resets sb */
void bm_err_sb_line(bm_sb *sb);          /* same to stderr (unbuffered) */
void bm_out_flush(void);

/* ------------------------------------------------------------------ math */

double bm_math_round(double x);           /* JS Math.round: half toward +infinity */
bm_int bm_f64_to_int(double x, const char *what, const char *loc);   /* traps on NaN/±inf/out of i64 range */
double bm_random(void);                   /* xorshift, deterministic seed unless BARM_SEED is set */

/* ------------------------------------------------------------------ closures and boxes */

typedef struct bm_env {
    int64_t rc;
    void (*drop)(struct bm_env *env);     /* releases captured values; runtime frees env after */
} bm_env;

typedef struct bm_fn {
    void *fn;          /* C function taking (bm_env *env, args...) */
    bm_env *env;       /* NULL when nothing is captured */
} bm_fn;

static inline void bm_env_retain(bm_env *e) { if (e && e->rc >= 0) e->rc++; }
void bm_env_release_slow(bm_env *e);
static inline void bm_env_release(bm_env *e) { if (e && e->rc > 0 && --e->rc == 0) bm_env_release_slow(e); }

/* ------------------------------------------------------------------ async (tasks, promises, timers)
 *
 * An async function compiles to a frame and a resume function that returns true once the frame
 * has finished. A task runs one root frame; awaited async calls are embedded in their caller's
 * frame, so resuming a task resumes its whole chain. Scheduling follows JavaScript exactly:
 * microtasks run in FIFO order after each macrotask (a timer callback, a request), and every
 * `await` costs one microtask tick. The one shortcut, `bm_async_eager`, is taken only when
 * running on immediately is indistinguishable from waiting a tick. Single-threaded. */

enum { BM_PENDING = 0, BM_FULFILLED = 1, BM_REJECTED = 2 };

typedef struct bm_task bm_task;

/* Something to run (as a microtask) when a promise settles. */
typedef struct bm_reaction { void (*fn)(void *a, void *b); void *a, *b; } bm_reaction;

typedef struct bm_promise {
    int64_t rc;
    uint8_t state;            /* BM_PENDING, BM_FULFILLED or BM_REJECTED */
    bool handled;             /* awaited: a rejection isn't reported as unhandled */
    bool reported;            /* on the unhandled-rejection list */
    const bm_type *vt;        /* the value's type */
    void *err;                /* rejection: an Error object (owned) */
    bm_task *waiter;          /* the task waiting for it, when that's the first and only reaction */
    bm_reaction *more;        /* other reactions, in the order they were added */
    int32_t nmore, capmore;
    _Alignas(16) unsigned char value[];
} bm_promise;

/* A task's `run` resumes its root frame; true once it has finished (and settled `promise`). */
typedef bool (*bm_task_run)(bm_task *t);

struct bm_task {
    bm_task *next;            /* microtask queue link */
    bm_task_run run;
    bm_promise *promise;      /* the task's result (the task holds one reference) */
    uint32_t flags;           /* BM_TASK_SYNC: its starter is still running (see bm_async_eager) */
    uint32_t size;            /* frame size */
    _Alignas(16) unsigned char frame[];
};
enum { BM_TASK_SYNC = 1u };

extern bm_task *bm_cur_task;  /* the task running now (NULL outside tasks) */

/* The program's error objects: released and printed through hooks the program installs. */
void bm_async_init(void (*err_retain)(void *), void (*err_release)(void *), void (*err_report)(void *));

bm_promise *bm_promise_new(const bm_type *vt);                 /* pending, rc 1 */
static inline void bm_promise_retain(bm_promise *p) { if (p) p->rc++; }
void bm_promise_release_slow(bm_promise *p);
static inline void bm_promise_release(bm_promise *p) { if (p && --p->rc == 0) bm_promise_release_slow(p); }
void bm_promise_resolve(bm_promise *p, const void *value);     /* copies and retains the value */
void bm_promise_resolve_move(bm_promise *p, void *value);      /* takes the value (no retain) */
void bm_promise_reject(bm_promise *p, void *err);              /* takes the error */
static inline void *bm_promise_value(bm_promise *p) { return p->value; }
bm_env *bm_promise_resolver(bm_promise *p);                     /* env of a `resolve`/`reject` closure: holds p */
static inline bm_promise *bm_resolver_promise(bm_env *env) { return *(bm_promise **)(env + 1); }

bm_task *bm_task_new(size_t frame_size, bm_task_run run, const bm_type *vt);   /* zeroed frame */
static inline void *bm_task_frame(bm_task *t) { return t->frame; }
/* Starts a task (an async call that isn't awaited): runs it until it first waits, then returns
 * its promise (a new reference). */
bm_promise *bm_task_spawn(bm_task *t);
/* Starts a task from the event loop (the program's top level): it may continue eagerly. */
void bm_task_start(bm_task *t);

/* Continuing now is indistinguishable from waiting a tick: no microtask is queued, and the task
 * isn't being run synchronously by a starter that still has code to run. */
extern size_t bm_mq_len;
static inline bool bm_async_eager(void) { return bm_mq_len == 0 && bm_cur_task && !(bm_cur_task->flags & BM_TASK_SYNC); }
/* `await p`: true if the value can be taken now — settled, and nothing else could run first.
 * Otherwise call bm_await_suspend and suspend: the task resumes once the value is ready. */
static inline bool bm_await_now(bm_promise *p) { p->handled = true; return p->state != BM_PENDING && bm_async_eager(); }
void bm_await_suspend(bm_promise *p);
/* After an embedded async call finished: suspend for the tick its promise would have taken. */
void bm_task_yield(void);

void bm_queue_microtask(bm_fn callback);        /* queueMicrotask(cb) (retains cb) */
/* Promise.all / Promise.race over an array of promises (borrowed): `et` describes their values;
 * `arr_t`, an array of them (all's result). A new promise (rc 1). */
bm_promise *bm_promise_all(struct bm_arr ps, const bm_type *et, const bm_type *arr_t);
bm_promise *bm_promise_race(struct bm_arr ps, const bm_type *et);
bm_int bm_set_timer(bm_fn callback, double ms, bool repeat);   /* setTimeout/setInterval (retains cb) */
void bm_clear_timer(bm_int id);
/* Runs the event loop until nothing is left: microtasks, timers and servers. */
void bm_async_run(void);

/* ------------------------------------------------------------------ tests */

/* Runs one test; a failing expect or a trap inside it marks it failed and returns.
 * Prints "ok   <name>" or "FAIL <name>\n  <message>\n  at <loc>". */
void bm_test_run(const char *name, void (*fn)(void));
bool bm_test_active(void);   /* inside bm_test_run (a trap fails the test instead of exiting) */
_Noreturn void bm_expect_fail(bm_sb *message, const char *loc);   /* message: e.g. "expected 3, received 4"; takes (frees) the builder */
int bm_test_summary(void);   /* prints "<n> passed, <m> failed"; returns exit status (0 if all passed) */

/* Called by the generated main() before anything else. Stores argc/argv below. */
void bm_init(int argc, char **argv);
extern int bm_argc;
extern char **bm_argv;

/* ------------------------------------------------------------------ system (used by the standard library) */

/* Errors are reported through a per-process slot read by bm_native_takeError (Node-style messages:
 * "ENOENT: no such file or directory, open 'x'"). */
bm_str bm_native_takeError(void);
bm_str bm_native_readFile(bm_str path);
void bm_native_writeFile(bm_str path, bm_str data, bool append);
bool bm_native_exists(bm_str path);
struct bm_arr bm_native_readDir(bm_str path);          /* names, sorted */
void bm_native_mkdir(bm_str path, bool recursive);
void bm_native_unlink(bm_str path);
void bm_native_rm(bm_str path, bool recursive, bool force);
bm_str bm_native_cwd(void);
struct bm_arr bm_process_argv(void);                   /* ["barm-binary-path", "program", args...] like Node's */
bool bm_process_env(bm_str name, bm_str *out);
_Noreturn void bm_process_exit(bm_int code);
double bm_date_now(void);                              /* ms since the epoch */
double bm_performance_now(void);                       /* ms since the program started */
void bm_write_stdout(bm_str s);
void bm_write_stderr(bm_str s);

/* ------------------------------------------------------------------ JSON */

void bm_json_quote(bm_sb *sb, bm_str s);               /* JSON.stringify of a string */
void bm_json_number(bm_sb *sb, double x);              /* JS number, or null for NaN/Infinity */
typedef struct bm_jp {
    const unsigned char *s;
    size_t n, i;
    char err[160];                                     /* first error ("" if none) */
} bm_jp;
void bm_jp_init(bm_jp *p, bm_str text);
char bm_jp_peek(bm_jp *p);                             /* next non-space byte, or 0 at the end */
bool bm_jp_char(bm_jp *p, char c);                     /* consumes c (after spaces) or fails */
bool bm_jp_try_char(bm_jp *p, char c);                 /* consumes c if it's next */
bool bm_jp_word(bm_jp *p, const char *w);              /* null / true / false */
bool bm_jp_string(bm_jp *p, bm_str *out);
bool bm_jp_number(bm_jp *p, double *out);
bool bm_jp_skip(bm_jp *p);                             /* any value */
bool bm_jp_end(bm_jp *p);                              /* only spaces left */
bool bm_jp_fail(bm_jp *p, const char *expected);       /* records "expected X at position N"; returns false */
bool bm_jp_find_key(bm_jp *p, const char *key, bm_str *val);  /* string value of key in the object at the cursor (not consumed) */
bm_str bm_jp_error(bm_jp *p);

/* ------------------------------------------------------------------ HTTP server (std/http) */

/* HTTP servers (std/http): httpListen binds and registers a server (returns its id, or -1 with
 * the error in bm_native_takeError); bm_http_run, called after the program, serves every
 * registered server — in httpWorkers processes — until all are stopped. For each request the
 * handler(env, method, target, headers, body) answers with bm_native_httpRespond. */
bm_int bm_native_httpListen(bm_int port, bm_str host, bm_fn handler);
bm_int bm_native_httpPort(bm_int id);
void bm_native_httpStop(bm_int id, bool force);
void bm_native_httpWorkers(bm_int n);
/* URLs: parse → [protocol, hostname, port, pathname, search, hash] (empty if invalid; `base`
 * resolves a relative input, "" for none); percent-decoding (`plus`: '+' is a space) and
 * application/x-www-form-urlencoded encoding. */
struct bm_arr bm_native_urlParse(bm_str input, bm_str base);
bm_str bm_native_requestUrl(bm_str headers, bm_str target);   /* "http://" + Host + target */
bm_str bm_native_urlNormalize(bm_str input, bm_str base);      /* normalized href, "" if invalid */
bm_str bm_native_urlPart(bm_str href, bm_int k);               /* 0 protocol .. 5 hash */
bm_str bm_native_urlDecode(bm_str s, bool plus);
bm_str bm_native_urlEncode(bm_str s);
void bm_native_httpRespond(bm_int status, bm_str headers, bm_str body, bool typed);  /* typed: add the default content-type */
/* An async handler: httpDefer (while the request is being handled) returns an id; the response
 * comes later through httpRespondTo. Responses on a connection go out in request order. */
bm_int bm_native_httpDefer(void);
/* The next task started (an async call not awaited) is in tail position: the caller does nothing
 * observable before the next microtask checkpoint, so the task may continue eagerly. */
void bm_native_spawnTail(void);
void bm_native_httpRespondTo(bm_int id, bm_int status, bm_str headers, bm_str body, bool typed);
bm_int bm_native_headerIndex(bm_str block, bm_str name);   /* offset of the value, or -1 */
bm_str bm_native_headerValue(bm_str block, bm_int at);
void bm_http_run(void);   /* after the program: serves registered servers until stopped (no-op if none) */
bm_str bm_native_headerRemove(bm_str block, bm_str name);  /* the block without `name` lines */
bm_str bm_native_headerAppend(bm_str block, bm_str name, bm_str value);

#endif /* BARM_H */

/* js.h — Barm's bridge to JavaScriptCore, for npm packages and the Node.js built-ins under them.
 *
 * A program that imports npm packages embeds their bundle (see crates/barm/src/npm) and links
 * runtime/js.c and runtime/node.c with JavaScriptCore. One context per process, created on the
 * first use of a package. Single-threaded, like the rest of the runtime.
 *
 * Values: a Barm `Js` value is a JSValueRef. JavaScriptCore's 64-bit JSValueRef is the encoded
 * value (as Bun relies on too), so numbers, booleans, undefined and null are made and read here
 * without calling the engine (bm_js() checks the encoding once; if it ever differs, everything
 * goes through the API). While Barm holds a cell (an object or a string) in a variable, field
 * or container, it's in a root table kept on the program's main stack, which the collector
 * scans conservatively: no engine call either (JSValueProtect takes over if the table fills).
 *
 * Microtasks: JavaScriptCore runs its microtask queue whenever the outermost call into
 * JavaScript returns (an evaluation, a call, a resolve), so promise reactions run right after
 * the native code that triggered them, as in Node.js. bm_js_drain is for the rare case of work
 * queued with no call in progress.
 *
 * This header declares the few JavaScriptCore types it needs instead of including its headers
 * (generated code compiles faster); js.c and node.c include the real ones. */
#ifndef BARM_JS_H
#define BARM_JS_H

#include "barm.h"

/* The same typedefs as <JavaScriptCore/JSBase.h> (repeating a typedef is allowed in C11). */
typedef const struct OpaqueJSContextGroup *JSContextGroupRef;
typedef const struct OpaqueJSContext *JSContextRef;
typedef struct OpaqueJSContext *JSGlobalContextRef;
typedef struct OpaqueJSString *JSStringRef;
typedef struct OpaqueJSClass *JSClassRef;
typedef const struct OpaqueJSValue *JSValueRef;
typedef struct OpaqueJSValue *JSObjectRef;
typedef JSValueRef (*JSObjectCallAsFunctionCallback)(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                                     size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception);

/* ------------------------------------------------------------------ the engine */

/* The program's bundle (generated code embeds it): the npm packages it imports and the Node.js
 * built-ins they use — a prelude that defines globalThis.__barm_npm, and each module's code,
 * compiled when it's first required (layout: Bundle::blob in crates/barm/src/npm/bundle.rs). */
extern const unsigned char bm_js_blob[];

/* The context: created on first use, with globalThis.__barm_native (filled by bm_node_install),
 * globalThis.__barm_source, and the bundle evaluated. */
JSContextRef bm_js(void);

/* Filled by runtime/node.c: the natives Node.js's built-ins run on (process, timers, fs, ...),
 * as functions on `native` (globalThis.__barm_native). */
void bm_node_install(JSContextRef ctx, JSObjectRef native);
/* obj[name] = a function calling fn. */
void bm_js_def(JSContextRef ctx, JSObjectRef obj, const char *name, JSObjectCallAsFunctionCallback fn);

/* Set by runtime/napi.c when the program links it (native addons). */
extern void (*bm_js_napi_install)(JSContextRef ctx);

/* Runs JavaScript's pending microtasks (see above). */
void bm_js_drain(void);

/* ------------------------------------------------------------------ values */

extern JSGlobalContextRef bm_js_ctx;   /* NULL until bm_js() */
extern bool bm_js_encoded;             /* the engine encodes values as below (checked by bm_js()) */

#define BM_JS_NUMBER_TAG 0xfffe000000000000ull
#define BM_JS_DOUBLE_OFFSET 0x0002000000000000ull
#define BM_JS_NOT_CELL 0xfffe000000000002ull
#define BM_JS_FALSE 0x06ull
#define BM_JS_TRUE 0x07ull
#define BM_JS_UNDEFINED 0x0aull
#define BM_JS_NULL 0x02ull

static inline uint64_t bm_js_bits(JSValueRef v) { uint64_t u; memcpy(&u, &v, 8); return u; }
static inline JSValueRef bm_js_of_bits(uint64_t u) { JSValueRef v; memcpy(&v, &u, 8); return v; }
/* An object, string, symbol or bigint: what the collector must be told Barm holds. */
static inline bool bm_js_is_cell(JSValueRef v) {
    uint64_t u = bm_js_bits(v);
    return bm_js_encoded ? u != 0 && (u & BM_JS_NOT_CELL) == 0 : u != 0;
}

void bm_js_root_add(JSValueRef v);
void bm_js_root_remove(JSValueRef v);
/* The program's main frame gives the root table its storage (see above). */
void bm_js_roots_init(JSValueRef *keys, uint32_t cap);
static inline JSValueRef bm_js_retain(JSValueRef v) { if (bm_js_is_cell(v)) bm_js_root_add(v); return v; }
static inline void bm_js_release(JSValueRef v) { if (bm_js_is_cell(v)) bm_js_root_remove(v); }

extern const bm_type bm_type_js;       /* Barm's `Js` (retain/release as above; String(x); inspect) */

JSValueRef bm_js_num_slow(double d);
JSValueRef bm_js_simple_slow(uint64_t bits);   /* booleans, undefined, null through the API */
static inline JSValueRef bm_js_undefined(void) { return bm_js_encoded ? bm_js_of_bits(BM_JS_UNDEFINED) : bm_js_simple_slow(BM_JS_UNDEFINED); }
static inline JSValueRef bm_js_null(void) { return bm_js_encoded ? bm_js_of_bits(BM_JS_NULL) : bm_js_simple_slow(BM_JS_NULL); }
static inline JSValueRef bm_js_bool(bool b) { return bm_js_encoded ? bm_js_of_bits(b ? BM_JS_TRUE : BM_JS_FALSE) : bm_js_simple_slow(b ? BM_JS_TRUE : BM_JS_FALSE); }
static inline JSValueRef bm_js_num(double d) {
    if (!bm_js_encoded) return bm_js_num_slow(d);
    uint64_t u;
    memcpy(&u, &d, 8);
    if (d >= -2147483648.0 && d <= 2147483647.0) {
        int32_t i = (int32_t)d;
        if ((double)i == d && u != 0x8000000000000000ull) return bm_js_of_bits(BM_JS_NUMBER_TAG | (uint32_t)i);
    }
    if (d != d) u = 0x7ff8000000000000ull;   /* the canonical NaN */
    return bm_js_of_bits(u + BM_JS_DOUBLE_OFFSET);
}
static inline bool bm_js_is_nullish(JSValueRef v) {
    uint64_t u = bm_js_bits(v);
    return u == BM_JS_UNDEFINED || u == BM_JS_NULL;
}
/* UTF-8 → a JavaScript string (invalid UTF-8 becomes U+FFFD). */
JSValueRef bm_js_str(const char *s, size_t n);
/* A Barm string (NUL-terminated; a literal's JavaScript string is made once and kept). */
JSValueRef bm_js_from_str(bm_str s);
/* String(v) as UTF-8 (lone surrogates become U+FFFD). Exceptions from toString are swallowed
 * into "[object]"-style text; use bm_js_call for code that may throw. */
bm_str bm_js_to_str(JSValueRef v);

/* typeof, and checked conversions: false if `v` isn't one (no coercion). */
const char *bm_js_typeof(JSValueRef v);    /* "undefined", "object", "function", "string", ... */
bool bm_js_as_num_slow(JSValueRef v, double *out);
bool bm_js_as_bool_slow(JSValueRef v, bool *out);
static inline bool bm_js_as_num(JSValueRef v, double *out) {
    if (!bm_js_encoded) return bm_js_as_num_slow(v, out);
    uint64_t u = bm_js_bits(v);
    if ((u & BM_JS_NUMBER_TAG) == BM_JS_NUMBER_TAG) {
        *out = (int32_t)(uint32_t)u;
        return true;
    }
    if ((u & BM_JS_NUMBER_TAG) == 0) return false;
    u -= BM_JS_DOUBLE_OFFSET;
    memcpy(out, &u, 8);
    return true;
}
static inline bool bm_js_as_bool(JSValueRef v, bool *out) {
    if (!bm_js_encoded) return bm_js_as_bool_slow(v, out);
    uint64_t u = bm_js_bits(v);
    if (u != BM_JS_TRUE && u != BM_JS_FALSE) return false;
    *out = u == BM_JS_TRUE;
    return true;
}
bool bm_js_as_str(JSValueRef v, bm_str *out);

/* ------------------------------------------------------------------ calls and properties
 *
 * Each returns the result, or NULL with *exc set to the thrown value (a JS exception). */

/* A property name as generated code uses it (`static bm_js_name k = {"name"};`): its string, and
 * small compiled functions reading the property and calling the method (one engine call each,
 * with the engine's own property caches). */
typedef struct bm_js_name {
    const char *text;
    JSStringRef str;
    JSValueRef value;          /* the name as a JavaScript string */
    JSObjectRef get;           /* (o) => o.name */
    JSObjectRef call[5];       /* (o, a0, ...) => o.name(a0, ...), by argument count */
} bm_js_name;
JSValueRef bm_js_name_get(JSValueRef obj, bm_js_name *k, JSValueRef *exc);
JSValueRef bm_js_name_call(JSValueRef obj, bm_js_name *k, size_t n, const JSValueRef *args, JSValueRef *exc);
/* A record as a JavaScript object: `make` is (a0, ...) => ({k0: a0, ...}), compiled on first use. */
typedef struct bm_js_shape {
    uint32_t n;
    const char *const *keys;
    const bool *optional;     /* (absent when undefined) */
    JSObjectRef make;
} bm_js_shape;
JSValueRef bm_js_shape_make(bm_js_shape *s, const JSValueRef *values);
/* A fused JavaScript expression (see codegen/js.rs): `return <expr>` over parameters a0, a1, ...,
 * compiled into *slot on first use; NULL with *exc set if it throws. */
JSValueRef bm_js_thunk(JSObjectRef *slot, const char *body, size_t n, const JSValueRef *args, JSValueRef *exc);
/* An exception where Barm code can't pass it on (a property read): traps. */
_Noreturn void bm_js_throw_trap(JSValueRef exc, const char *loc);

/* A property name, created once: `static JSStringRef k; bm_js_key(&k, "name")`. */
JSStringRef bm_js_key_slow(JSStringRef *slot, const char *name);
static inline JSStringRef bm_js_key(JSStringRef *slot, const char *name) { return *slot ? *slot : bm_js_key_slow(slot, name); }

JSValueRef bm_js_get(JSValueRef obj, JSStringRef key, JSValueRef *exc);
bool bm_js_set(JSValueRef obj, JSStringRef key, JSValueRef value, JSValueRef *exc);
JSValueRef bm_js_index(JSValueRef obj, uint32_t i, JSValueRef *exc);
/* fn(...args) with `this` (NULL: undefined). */
JSValueRef bm_js_call(JSValueRef fn, JSValueRef self, size_t n, const JSValueRef *args, JSValueRef *exc);
/* obj.key(...args) */
JSValueRef bm_js_invoke(JSValueRef obj, JSStringRef key, size_t n, const JSValueRef *args, JSValueRef *exc);
/* new fn(...args) */
JSValueRef bm_js_new(JSValueRef fn, size_t n, const JSValueRef *args, JSValueRef *exc);

/* A package's exports: require(spec) inside the bundle (spec as the program imports it). */
JSValueRef bm_js_import(const char *spec, JSValueRef *exc);

/* Set by the program: a Barm JsError object (owned) holding a thrown JavaScript value. */
extern void *(*bm_js_make_error)(JSValueRef exc);

/* A thrown value as text: "Name: message" and, for errors, the stack. */
bm_str bm_js_error_text(JSValueRef exc);
/* For a Barm JsError: the thrown value's name and message (an Error's own; else "JsError" and
 * String(value)). */
void bm_js_error_parts(JSValueRef exc, bm_str *name, bm_str *message);

/* ------------------------------------------------------------------ for generated code */

/* `import ... from "spec"`: kind 0 the namespace (`import * as`), 1 the default export, else
 * the module's exports; traps if the package fails to load. */
JSValueRef bm_js_import_as(const char *spec, int kind, const char *loc);
/* obj.key / obj[i] / obj[k]; a JavaScript exception (a throwing getter, a property of
 * undefined) traps. */
JSValueRef bm_js_get_or_trap(JSValueRef obj, bm_js_name *key, const char *loc);
JSValueRef bm_js_at_or_trap(JSValueRef obj, double i, const char *loc);
JSValueRef bm_js_key_or_trap(JSValueRef obj, bm_str key, const char *loc);
void bm_js_put_or_trap(JSValueRef obj, bm_js_name *key, JSValueRef value, const char *loc);
/* A value from JavaScript that doesn't convert to the type `want` names: traps. */
_Noreturn void bm_js_type_trap(JSValueRef v, const char *want, const char *loc);
bool bm_js_truthy_slow(JSValueRef v);
static inline bool bm_js_truthy(JSValueRef v) {
    if (!bm_js_encoded || bm_js_is_cell(v)) return bm_js_truthy_slow(v);
    uint64_t u = bm_js_bits(v);
    if ((u & BM_JS_NUMBER_TAG) == BM_JS_NUMBER_TAG) return (uint32_t)u != 0;
    if (u & BM_JS_NUMBER_TAG) {
        double d;
        u -= BM_JS_DOUBLE_OFFSET;
        memcpy(&d, &u, 8);
        return d == d && d != 0;
    }
    return u == BM_JS_TRUE;
}
bool bm_js_is_array(JSValueRef v);
uint32_t bm_js_length(JSValueRef v);    /* v.length as an array index */
JSValueRef bm_js_array(size_t n, const JSValueRef *items);
JSValueRef bm_js_object(void);
JSValueRef bm_js_lit(JSValueRef *slot, const char *s, size_t n);   /* a string literal, created once */
/* A JavaScript function calling a Barm closure through `tramp` (generated per signature); the
 * closure's environment is held until the function is collected. */
typedef JSValueRef (*bm_js_tramp)(const bm_fn *fn, size_t n, const JSValueRef *args);
JSValueRef bm_js_function(bm_fn fn, bm_js_tramp tramp);

/* ------------------------------------------------------------------ bytes */

/* A Uint8Array over ptr[0, len) without copying; dealloc(ptr, ctx) runs when it's collected
 * (NULL: never). */
JSObjectRef bm_js_bytes(void *ptr, size_t len, void (*dealloc)(void *bytes, void *ctx), void *ctx);
/* A copy of bytes in a new Uint8Array. */
JSObjectRef bm_js_bytes_copy(const void *ptr, size_t len);
/* The bytes of a typed array, ArrayBuffer or DataView (valid until JavaScript next runs). */
bool bm_js_bytes_view(JSValueRef v, uint8_t **ptr, size_t *len);

/* ------------------------------------------------------------------ promises */

/* A pending JavaScript promise that native code settles later (once). */
typedef struct bm_js_deferred bm_js_deferred;
JSObjectRef bm_js_deferred_new(bm_js_deferred **out);
void bm_js_settle(bm_js_deferred *d, JSValueRef value, bool ok);   /* resolves (ok) or rejects; frees d */

/* Barm awaiting a JavaScript value: a Barm promise (of Js) settled when `v` settles (now, if `v`
 * isn't a thenable). A rejection becomes a JsError holding the reason. */
bm_promise *bm_js_await(JSValueRef v);

#endif /* BARM_JS_H */

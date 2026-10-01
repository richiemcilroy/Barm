/* js.h — Barm's bridge to JavaScriptCore, for npm packages and the Node.js built-ins under them.
 *
 * A program that imports npm packages embeds their bundle (see crates/barm/src/npm) and links
 * runtime/js.c and runtime/node.c with JavaScriptCore. One context per process, created on the
 * first use of a package. Single-threaded, like the rest of the runtime.
 *
 * Values: a Barm `Js` value is a JSValueRef. While Barm owns one (in a variable, a field, a
 * container) it is protected from the collector: retain is JSValueProtect, release
 * JSValueUnprotect (no-ops for numbers, booleans, null and undefined). Values on the C stack
 * needn't be: the collector scans the stack.
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

/* Runs JavaScript's pending microtasks (see above). */
void bm_js_drain(void);

/* ------------------------------------------------------------------ values */

void JSValueProtect(JSContextRef ctx, JSValueRef value);
void JSValueUnprotect(JSContextRef ctx, JSValueRef value);
extern JSGlobalContextRef bm_js_ctx;   /* NULL until bm_js() */
static inline JSValueRef bm_js_retain(JSValueRef v) { JSValueProtect(bm_js_ctx, v); return v; }
static inline void bm_js_release(JSValueRef v) { if (v) JSValueUnprotect(bm_js_ctx, v); }

extern const bm_type bm_type_js;       /* Barm's `Js` (retain/release as above; String(x); inspect) */

JSValueRef bm_js_undefined(void);
JSValueRef bm_js_null(void);
JSValueRef bm_js_bool(bool b);
JSValueRef bm_js_num(double d);
/* UTF-8 → a JavaScript string (invalid UTF-8 becomes U+FFFD). */
JSValueRef bm_js_str(const char *s, size_t n);
static inline JSValueRef bm_js_from_str(bm_str s) { return bm_js_str(s.p->data, (size_t)s.p->len); }
/* String(v) as UTF-8 (lone surrogates become U+FFFD). Exceptions from toString are swallowed
 * into "[object]"-style text; use bm_js_call for code that may throw. */
bm_str bm_js_to_str(JSValueRef v);

/* typeof, and checked conversions: false if `v` isn't one (no coercion). */
const char *bm_js_typeof(JSValueRef v);    /* "undefined", "object", "function", "string", ... */
bool bm_js_as_num(JSValueRef v, double *out);
bool bm_js_as_bool(JSValueRef v, bool *out);
bool bm_js_as_str(JSValueRef v, bm_str *out);

/* ------------------------------------------------------------------ calls and properties
 *
 * Each returns the result, or NULL with *exc set to the thrown value (a JS exception). */

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

/* A thrown value as text: "Name: message" and, for errors, the stack. */
bm_str bm_js_error_text(JSValueRef exc);

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

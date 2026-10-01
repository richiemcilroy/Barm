/* js.c — Barm's bridge to JavaScriptCore (see js.h): the engine, values, calls, bytes and
 * promises across the boundary. Linked only by programs that import npm packages, with
 * runtime/node.c (the Node.js natives) and the system JavaScriptCore framework. */

#include "js.h"

#include <JavaScriptCore/JavaScriptCore.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

JSGlobalContextRef bm_js_ctx;
bool bm_js_encoded;
static JSStringRef bm_js_bundle_url;    /* the prelude's URL */
static JSObjectRef bm_js_npm;           /* globalThis.__barm_npm (protected) */
static JSObjectRef bm_js_noop;          /* calling it runs pending microtasks */

/* Set by the program: a Barm JsError object (owned) holding a thrown JavaScript value. */
void *(*bm_js_make_error)(JSValueRef exc);

/* Until runtime/node.c defines it (a program links node.c as an object, so its definition wins):
 * no natives, so the shims fall back to what the engine has (internal/bootstrap/host_fallback). */
__attribute__((weak)) void bm_node_install(JSContextRef ctx, JSObjectRef native) {
    (void)native;
    JSStringRef k = JSStringCreateWithUTF8CString("__barm_native");
    JSObjectDeleteProperty(ctx, JSContextGetGlobalObject(ctx), k, NULL);
    JSStringRelease(k);
}

static JSValueRef bm_js_noop_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)n; (void)a; (void)exc;
    return JSValueMakeUndefined(ctx);
}

/* The blob (see Bundle::blob in crates/barm/src/npm/bundle.rs): a u32 index — the module count,
 * the prelude's offset, then per module its name's offset, its source's offset and length — and
 * NUL-terminated strings. */
static uint32_t bm_js_u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t bm_js_nmodules(void) { return bm_js_u32(bm_js_blob); }
static const char *bm_js_prelude(void) { return (const char *)bm_js_blob + bm_js_u32(bm_js_blob + 4); }
static const char *bm_js_module_name(uint32_t i) { return (const char *)bm_js_blob + bm_js_u32(bm_js_blob + 8 + 16 * i); }
static const char *bm_js_module_src(uint32_t i, size_t *len) {
    *len = bm_js_u32(bm_js_blob + 16 + 16 * i);
    return (const char *)bm_js_blob + bm_js_u32(bm_js_blob + 12 + 16 * i);
}
static const char *bm_js_module_map(uint32_t i) { return (const char *)bm_js_blob + bm_js_u32(bm_js_blob + 20 + 16 * i); }

static JSStringRef bm_js_string_ref(const char *s, size_t n, bool nul_terminated);

/* globalThis.__barm_name(id) / __barm_map(id): a module's name, and its requires (encoded, see
 * Bundle::maps), read from the blob when the module is first loaded. */
static JSValueRef bm_js_blob_string(JSContextRef ctx, size_t n, const JSValueRef a[], const char *(*get)(uint32_t)) {
    double d = n > 0 ? JSValueToNumber(ctx, a[0], NULL) : -1;
    if (!(d >= 0 && d < bm_js_nmodules())) return JSValueMakeUndefined(ctx);
    const char *s = get((uint32_t)d);
    JSStringRef t = bm_js_string_ref(s, strlen(s), true);
    JSValueRef v = JSValueMakeString(ctx, t);
    JSStringRelease(t);
    return v;
}
static JSValueRef bm_js_name_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    return bm_js_blob_string(ctx, n, a, bm_js_module_name);
}
static JSValueRef bm_js_map_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    return bm_js_blob_string(ctx, n, a, bm_js_module_map);
}

static JSStringRef bm_js_string_ref(const char *s, size_t n, bool nul_terminated);

/* A module's source as a JavaScript string. */
static JSStringRef bm_js_module_text(uint32_t i) {
    size_t len;
    const char *src = bm_js_module_src(i, &len);
    /* (the bundler's sources are valid UTF-8; a NUL byte inside needs the slow path) */
    if (strlen(src) == len) return JSStringCreateWithUTF8CString(src);
    return bm_js_string_ref(src, len, false);
}

/* globalThis.__barm_compile(id): module `id`'s function, compiled on first require with the
 * module's name as its URL (stack traces name the file; line numbers are the file's). */
static JSValueRef bm_js_compile_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self;
    double d = n > 0 ? JSValueToNumber(ctx, a[0], NULL) : -1;
    if (!(d >= 0 && d < bm_js_nmodules())) return JSValueMakeUndefined(ctx);
    uint32_t i = (uint32_t)d;
    /* BARM_JS_TRACE=1 lists the modules as they're compiled (what a program loads at start) */
    static int trace = -1;
    if (trace < 0) trace = getenv("BARM_JS_TRACE") != NULL;
    double t0 = trace ? bm_performance_now() : 0;
    JSStringRef src = bm_js_module_text(i);
    JSStringRef url = JSStringCreateWithUTF8CString(bm_js_module_name(i));
    JSValueRef r = JSEvaluateScript(ctx, src, NULL, url, 1, exc);
    JSStringRelease(src);
    JSStringRelease(url);
    if (trace) {
        char line[512];
        size_t len;
        bm_js_module_src(i, &len);
        int k = snprintf(line, sizeof line, "barm: compiled %s (%zu bytes) in %.3f ms\n", bm_js_module_name(i), len, bm_performance_now() - t0);
        bm_write_fd(2, line, (size_t)k);
    }
    return r;
}

/* globalThis.__barm_read_file(path): a file's text (UTF-8), or undefined. For requires of files
 * the bundle doesn't hold (`require(dir + "/package.json")`): read where the program runs, as
 * Node.js does. */
static JSValueRef bm_js_read_file_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    if (n == 0 || !JSValueIsString(ctx, a[0])) return JSValueMakeUndefined(ctx);
    bm_str path = bm_js_to_str(a[0]);
    FILE *fp = fopen(path.p->data, "rb");
    bm_str_release(path);
    if (!fp) return JSValueMakeUndefined(ctx);
    bm_sb sb = {0};
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, fp)) > 0) bm_sb_push(&sb, buf, got);
    fclose(fp);
    JSStringRef t = bm_js_string_ref(sb.data ? sb.data : "", sb.len, false);
    bm_sb_free(&sb);
    JSValueRef v = JSValueMakeString(ctx, t);
    JSStringRelease(t);
    return v;
}

/* globalThis.__barm_compile_source(code, url): a CommonJS module function compiled from source
 * (a file read from disk, see above). */
static JSValueRef bm_js_compile_source_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self;
    if (n < 2) return JSValueMakeUndefined(ctx);
    JSStringRef code = JSValueToStringCopy(ctx, a[0], exc);
    JSStringRef url = JSValueToStringCopy(ctx, a[1], exc);
    if (!code || !url) return NULL;
    /* (function (module, exports, require, __filename, __dirname) {<code>\n}) */
    JSStringRef params[5] = {JSStringCreateWithUTF8CString("module"), JSStringCreateWithUTF8CString("exports"), JSStringCreateWithUTF8CString("require"), JSStringCreateWithUTF8CString("__filename"), JSStringCreateWithUTF8CString("__dirname")};
    JSObjectRef fn = JSObjectMakeFunction(ctx, NULL, 5, params, code, url, 1, exc);
    for (int i = 0; i < 5; i++) JSStringRelease(params[i]);
    JSStringRelease(code);
    JSStringRelease(url);
    return fn;
}

/* globalThis.__barm_source(url): the code of the module at `url` (assert reads the failing
 * expression from it), else undefined. */
static JSValueRef bm_js_source_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self;
    if (n == 0 || !JSValueIsString(ctx, a[0])) return JSValueMakeUndefined(ctx);
    bm_str url = bm_js_to_str(a[0]);
    JSValueRef r = JSValueMakeUndefined(ctx);
    if (strcmp(url.p->data, "barm:npm") == 0) {
        JSStringRef t = JSStringCreateWithUTF8CString(bm_js_prelude());
        r = JSValueMakeString(ctx, t);
        JSStringRelease(t);
    } else {
        for (uint32_t i = 0, n = bm_js_nmodules(); i < n; i++) {
            if (strcmp(bm_js_module_name(i), url.p->data) == 0) {
                JSStringRef t = bm_js_module_text(i);
                r = JSValueMakeString(ctx, t);
                JSStringRelease(t);
                break;
            }
        }
    }
    bm_str_release(url);
    (void)exc;
    return r;
}

void bm_js_def(JSContextRef ctx, JSObjectRef obj, const char *name, JSObjectCallAsFunctionCallback fn) {
    JSStringRef k = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, obj, k, JSObjectMakeFunctionWithCallback(ctx, k, fn), kJSPropertyAttributeDontEnum, NULL);
    JSStringRelease(k);
}

static _Noreturn void bm_js_fatal(const char *what, JSValueRef exc) {
    bm_str text = bm_js_error_text(exc);
    bm_err_cstr(what);
    bm_err_cstr(text.p->data);
    bm_err_cstr("\n");
    exit(1);
}

static void bm_js_trace_phase(const char *what, double since) {
    static int trace = -1;
    if (trace < 0) trace = getenv("BARM_JS_TRACE") != NULL;
    if (!trace) return;
    char line[160];
    int k = snprintf(line, sizeof line, "barm: %s in %.3f ms (%.3f ms since start)\n", what, bm_performance_now() - since, bm_performance_now());
    bm_write_fd(2, line, (size_t)k);
}

JSContextRef bm_js(void) {
    if (bm_js_ctx) return bm_js_ctx;
    double t0 = bm_performance_now();
    /* SharedArrayBuffer, which JavaScriptCore leaves out of API contexts unless asked: its
     * options come from the environment when the first VM starts, and only then (so the program
     * and its children never see the variable) */
    bool sab = !getenv("JSC_useSharedArrayBuffer");
    if (sab) setenv("JSC_useSharedArrayBuffer", "1", 0);
    JSGlobalContextRef ctx = JSGlobalContextCreate(NULL);
    if (sab) unsetenv("JSC_useSharedArrayBuffer");
    bm_js_trace_phase("created the JavaScript context", t0);
    bm_js_ctx = ctx;
    /* values made without the engine (js.h): only if it encodes them as expected */
    bm_js_encoded = bm_js_bits(JSValueMakeNumber(ctx, 1.5)) == 0x3ffa000000000000ull && bm_js_bits(JSValueMakeNumber(ctx, -1)) == 0xfffe0000ffffffffull
        && bm_js_bits(JSValueMakeBoolean(ctx, true)) == BM_JS_TRUE && bm_js_bits(JSValueMakeBoolean(ctx, false)) == BM_JS_FALSE
        && bm_js_bits(JSValueMakeUndefined(ctx)) == BM_JS_UNDEFINED && bm_js_bits(JSValueMakeNull(ctx)) == BM_JS_NULL;
    JSObjectRef global = JSContextGetGlobalObject(ctx);
    JSObjectRef native = JSObjectMake(ctx, NULL, NULL);
    JSStringRef k = JSStringCreateWithUTF8CString("__barm_native");
    JSObjectSetProperty(ctx, global, k, native, kJSPropertyAttributeDontEnum, NULL);
    JSStringRelease(k);
    bm_node_install(ctx, native);
    bm_js_def(ctx, global, "__barm_source", bm_js_source_fn);
    bm_js_def(ctx, global, "__barm_compile", bm_js_compile_fn);
    bm_js_def(ctx, global, "__barm_name", bm_js_name_fn);
    bm_js_def(ctx, global, "__barm_read_file", bm_js_read_file_fn);
    bm_js_def(ctx, global, "__barm_compile_source", bm_js_compile_source_fn);
    bm_js_def(ctx, global, "__barm_map", bm_js_map_fn);
    {
        JSStringRef k = JSStringCreateWithUTF8CString("__barm_count");
        JSObjectSetProperty(ctx, global, k, JSValueMakeNumber(ctx, bm_js_nmodules()), kJSPropertyAttributeDontEnum, NULL);
        JSStringRelease(k);
    }
    bm_js_noop = JSObjectMakeFunctionWithCallback(ctx, NULL, bm_js_noop_fn);
    JSValueProtect(ctx, bm_js_noop);
    JSStringRef prelude = JSStringCreateWithUTF8CString(bm_js_prelude());
    bm_js_bundle_url = JSStringCreateWithUTF8CString("barm:npm");
    JSValueRef exc = NULL;
    double t1 = bm_performance_now();
    JSEvaluateScript(ctx, prelude, NULL, bm_js_bundle_url, 1, &exc);
    bm_js_trace_phase("ran the bundle's prelude (and the globals)", t1);
    JSStringRelease(prelude);
    if (exc) bm_js_fatal("npm packages failed to load: ", exc);
    k = JSStringCreateWithUTF8CString("__barm_npm");
    JSValueRef npm = JSObjectGetProperty(ctx, global, k, NULL);
    JSStringRelease(k);
    bm_js_npm = (JSObjectRef)npm;
    JSValueProtect(ctx, npm);
    return ctx;
}

void bm_js_drain(void) {
    if (bm_js_ctx) JSObjectCallAsFunction(bm_js_ctx, bm_js_noop, NULL, 0, NULL, NULL);
}

/* ------------------------------------------------------------------ values */


/* UTF-8 → UTF-16, invalid sequences as U+FFFD (WHATWG decoding). Returns the length. */
static size_t bm_utf8_to_utf16(const unsigned char *s, size_t n, JSChar *out) {
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c < 0x80) { out[o++] = (JSChar)c; i++; continue; }
        unsigned need, cp, lo = 0x80, hi = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) { need = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; cp = c & 0x0f; if (c == 0xe0) lo = 0xa0; if (c == 0xed) hi = 0x9f; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; cp = c & 0x07; if (c == 0xf0) lo = 0x90; if (c == 0xf4) hi = 0x8f; }
        else { out[o++] = 0xfffd; i++; continue; }
        i++;
        bool bad = false;
        for (unsigned k = 0; k < need; k++) {
            if (i >= n || s[i] < lo || s[i] > hi) { bad = true; break; }
            cp = (cp << 6) | (s[i] & 0x3f);
            lo = 0x80; hi = 0xbf;
            i++;
        }
        if (bad) { out[o++] = 0xfffd; continue; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[o++] = (JSChar)(0xd800 + (cp >> 10));
            out[o++] = (JSChar)(0xdc00 + (cp & 0x3ff));
        } else {
            out[o++] = (JSChar)cp;
        }
    }
    return o;
}

/* Valid UTF-8 without NUL bytes (JavaScriptCore reads it as a C string, 8-bit when it can). */
static bool bm_utf8_plain(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c >= 0x01 && c < 0x80) { i++; continue; }
        if (c < 0xc2 || c > 0xf4) return false;   /* (NUL included) */
        size_t need = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
        if (n - i <= need) return false;
        unsigned lo = 0x80, hi = 0xbf;
        if (c == 0xe0) lo = 0xa0; else if (c == 0xed) hi = 0x9f; else if (c == 0xf0) lo = 0x90; else if (c == 0xf4) hi = 0x8f;
        if (s[i + 1] < lo || s[i + 1] > hi) return false;
        for (size_t k = 2; k <= need; k++) if ((s[i + k] & 0xc0) != 0x80) return false;
        i += need + 1;
    }
    return true;
}

static JSStringRef bm_js_string_ref(const char *s, size_t n, bool nul_terminated) {
    if (nul_terminated && bm_utf8_plain((const unsigned char *)s, n)) return JSStringCreateWithUTF8CString(s);
    JSChar small[256];
    JSChar *buf = n <= 256 ? small : bm_alloc(n * sizeof(JSChar));
    size_t len = bm_utf8_to_utf16((const unsigned char *)s, n, buf);
    JSStringRef r = JSStringCreateWithCharacters(buf, len);
    if (buf != small) bm_free(buf);
    return r;
}

JSValueRef bm_js_from_str(bm_str s) {
    JSContextRef ctx = bm_js();
    /* literals (immortal buffers): a small direct-mapped cache of their JavaScript strings */
    enum { LIT_CACHE = 256 };
    static struct { const bm_strbuf *p; JSValueRef v; } lits[LIT_CACHE];
    uint32_t slot = 0;
    if (s.p->rc < 0) {
        slot = (uint32_t)(((uintptr_t)s.p >> 4) * 0x9e3779b97f4a7c15ull >> 56);
        if (lits[slot].p == s.p) return lits[slot].v;
    }
    JSStringRef r = bm_js_string_ref(s.p->data, (size_t)s.p->len, true);
    JSValueRef v = JSValueMakeString(ctx, r);
    JSStringRelease(r);
    if (s.p->rc < 0) {
        if (lits[slot].v) JSValueUnprotect(ctx, lits[slot].v);
        JSValueProtect(ctx, v);
        lits[slot].p = s.p;
        lits[slot].v = v;
    }
    return v;
}

JSValueRef bm_js_str(const char *s, size_t n) {
    JSContextRef ctx = bm_js();
    JSStringRef r = bm_js_string_ref(s, n, false);
    JSValueRef v = JSValueMakeString(ctx, r);
    JSStringRelease(r);
    return v;
}

static bm_str bm_js_string_to_str(JSStringRef r) {
    size_t len = JSStringGetLength(r);
    const JSChar *u = JSStringGetCharactersPtr(r);
    /* UTF-16 → UTF-8 with lone surrogates as U+FFFD (as TextEncoder does) */
    bm_sb sb = {0};
    bm_sb_grow(&sb, len + 1);
    for (size_t i = 0; i < len; i++) {
        unsigned c = u[i];
        if (c < 0x80) { bm_sb_add_char(&sb, (char)c); continue; }
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < len && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (u[i + 1] - 0xdc00);
            i++;
        } else if (c >= 0xd800 && c <= 0xdfff) {
            c = 0xfffd;
        }
        char b[4];
        size_t k;
        if (c < 0x800) { b[0] = (char)(0xc0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3f)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3f)); b[2] = (char)(0x80 | (c & 0x3f)); k = 3; }
        else { b[0] = (char)(0xf0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3f)); b[2] = (char)(0x80 | ((c >> 6) & 0x3f)); b[3] = (char)(0x80 | (c & 0x3f)); k = 4; }
        bm_sb_add(&sb, b, k);
    }
    return bm_str_from_sb(&sb);
}

bm_str bm_js_to_str(JSValueRef v) {
    JSContextRef ctx = bm_js();
    JSValueRef exc = NULL;
    JSStringRef r = JSValueToStringCopy(ctx, v, &exc);
    if (!r) return bm_str_from("[object]", 8);
    bm_str s = bm_js_string_to_str(r);
    JSStringRelease(r);
    return s;
}

const char *bm_js_typeof(JSValueRef v) {
    JSContextRef ctx = bm_js();
    switch (JSValueGetType(ctx, v)) {
    case kJSTypeUndefined: return "undefined";
    case kJSTypeNull: return "object";
    case kJSTypeBoolean: return "boolean";
    case kJSTypeNumber: return "number";
    case kJSTypeString: return "string";
    case kJSTypeSymbol: return "symbol";
#if defined(MAC_OS_VERSION_15_0) || defined(kJSTypeBigInt)
    case kJSTypeBigInt: return "bigint";
#endif
    case kJSTypeObject: return JSObjectIsFunction(ctx, (JSObjectRef)v) ? "function" : "object";
    }
    return "object";
}

JSValueRef bm_js_num_slow(double d) { return JSValueMakeNumber(bm_js(), d); }
JSValueRef bm_js_simple_slow(uint64_t bits) {
    JSContextRef ctx = bm_js();
    switch (bits) {
    case BM_JS_TRUE: return JSValueMakeBoolean(ctx, true);
    case BM_JS_FALSE: return JSValueMakeBoolean(ctx, false);
    case BM_JS_NULL: return JSValueMakeNull(ctx);
    default: return JSValueMakeUndefined(ctx);
    }
}

bool bm_js_as_num_slow(JSValueRef v, double *out) {
    JSContextRef ctx = bm_js();
    if (!JSValueIsNumber(ctx, v)) return false;
    *out = JSValueToNumber(ctx, v, NULL);
    return true;
}

bool bm_js_as_bool_slow(JSValueRef v, bool *out) {
    JSContextRef ctx = bm_js();
    if (!JSValueIsBoolean(ctx, v)) return false;
    *out = JSValueToBoolean(ctx, v);
    return true;
}

bool bm_js_as_str(JSValueRef v, bm_str *out) {
    JSContextRef ctx = bm_js();
    if (!JSValueIsString(ctx, v)) return false;
    JSStringRef r = JSValueToStringCopy(ctx, v, NULL);
    *out = bm_js_string_to_str(r);
    JSStringRelease(r);
    return true;
}

/* bm_type_js: Js values in Barm containers */
static void bm_js_t_retain(void *p) { bm_js_retain(*(JSValueRef *)p); }
static void bm_js_t_release(void *p) { bm_js_release(*(JSValueRef *)p); }
static bool bm_js_t_eq(const void *a, const void *b) { return JSValueIsStrictEqual(bm_js_ctx, *(JSValueRef *)a, *(JSValueRef *)b); }
static uint64_t bm_js_t_hash(const void *p) {
    JSValueRef v = *(JSValueRef *)p;
    JSContextRef ctx = bm_js_ctx;
    if (JSValueIsString(ctx, v)) {
        bm_str s = bm_js_to_str(v);
        uint64_t h = bm_str_hash(s);
        bm_str_release(s);
        return h;
    }
    if (JSValueIsNumber(ctx, v)) {
        double d = JSValueToNumber(ctx, v, NULL);
        if (d == 0) d = 0; /* -0 === 0 */
        uint64_t u;
        memcpy(&u, &d, 8);
        return u * 0x9e3779b97f4a7c15ull;
    }
    return (uint64_t)(uintptr_t)v * 0x9e3779b97f4a7c15ull;
}
static void bm_js_t_to_str(bm_sb *sb, const void *p) {
    bm_str s = bm_js_to_str(*(JSValueRef *)p);
    bm_sb_push_str(sb, s);
    bm_str_release(s);
}
static void bm_js_t_inspect(bm_sb *sb, const void *p, int depth) {
    JSValueRef v = *(JSValueRef *)p;
    JSContextRef ctx = bm_js();
    /* Node.js's util.inspect when the bundle has node:util (programs that print JavaScript values
     * do), else JSON-ish */
    static JSValueRef inspect;
    static bool looked;
    if (!looked) {
        looked = true;
        JSValueRef exc = NULL;
        JSValueRef util = bm_js_import("node:util", &exc);
        static JSStringRef k;
        if (util && JSValueIsObject(ctx, util)) inspect = JSObjectGetProperty(ctx, (JSObjectRef)util, bm_js_key(&k, "inspect"), NULL);
        if (inspect && JSValueIsObject(ctx, inspect)) JSValueProtect(ctx, inspect); else inspect = NULL;
    }
    if (depth == 0 && JSValueIsString(ctx, v)) {
        bm_js_t_to_str(sb, p);
        return;
    }
    if (inspect) {
        JSValueRef r = JSObjectCallAsFunction(ctx, (JSObjectRef)inspect, NULL, 1, &v, NULL);
        if (r && JSValueIsString(ctx, r)) {
            bm_str s = bm_js_to_str(r);
            bm_sb_push_str(sb, s);
            bm_str_release(s);
            return;
        }
    }
    if (JSValueIsString(ctx, v)) {
        bm_str s = bm_js_to_str(v);
        bm_inspect_str(sb, s, depth);
        bm_str_release(s);
        return;
    }
    if (JSValueIsObject(ctx, v) && !JSObjectIsFunction(ctx, (JSObjectRef)v)) {
        JSStringRef json = JSValueCreateJSONString(ctx, v, 0, NULL);
        if (json) {
            bm_str s = bm_js_string_to_str(json);
            JSStringRelease(json);
            bm_sb_push_str(sb, s);
            bm_str_release(s);
            return;
        }
    }
    bm_js_t_to_str(sb, p);
}
const bm_type bm_type_js = {sizeof(JSValueRef), bm_js_t_retain, bm_js_t_release, bm_js_t_eq, bm_js_t_hash, bm_js_t_to_str, bm_js_t_inspect};

/* ------------------------------------------------------------------ calls and properties */

JSStringRef bm_js_key_slow(JSStringRef *slot, const char *name) {
    *slot = JSStringCreateWithUTF8CString(name);
    return *slot;
}

static JSValueRef bm_js_type_error(JSContextRef ctx, const char *msg, JSValueRef *exc) {
    JSValueRef m = bm_js_str(msg, strlen(msg));
    JSObjectRef e = JSObjectMakeError(ctx, 1, &m, NULL);
    static JSStringRef kname;
    JSObjectSetProperty(ctx, e, bm_js_key(&kname, "name"), bm_js_str("TypeError", 9), kJSPropertyAttributeDontEnum, NULL);
    *exc = e;
    return NULL;
}

JSValueRef bm_js_get(JSValueRef obj, JSStringRef key, JSValueRef *exc) {
    JSContextRef ctx = bm_js();
    JSObjectRef o = JSValueToObject(ctx, obj, exc);
    if (!o) return NULL;
    JSValueRef r = JSObjectGetProperty(ctx, o, key, exc);
    return *exc ? NULL : r;
}

bool bm_js_set(JSValueRef obj, JSStringRef key, JSValueRef value, JSValueRef *exc) {
    JSContextRef ctx = bm_js();
    JSObjectRef o = JSValueToObject(ctx, obj, exc);
    if (!o) return false;
    JSObjectSetProperty(ctx, o, key, value, kJSPropertyAttributeNone, exc);
    return !*exc;
}

JSValueRef bm_js_index(JSValueRef obj, uint32_t i, JSValueRef *exc) {
    JSContextRef ctx = bm_js();
    JSObjectRef o = JSValueToObject(ctx, obj, exc);
    if (!o) return NULL;
    JSValueRef r = JSObjectGetPropertyAtIndex(ctx, o, i, exc);
    return *exc ? NULL : r;
}

JSValueRef bm_js_call(JSValueRef fn, JSValueRef self, size_t n, const JSValueRef *args, JSValueRef *exc) {
    JSContextRef ctx = bm_js();
    if (!JSValueIsObject(ctx, fn) || !JSObjectIsFunction(ctx, (JSObjectRef)fn)) return bm_js_type_error(ctx, "not a function", exc);
    JSObjectRef this_obj = NULL;
    if (self && !JSValueIsUndefined(ctx, self)) {
        /* (a primitive `this` is boxed, as for sloppy-mode callees) */
        this_obj = JSValueToObject(ctx, self, NULL);
    }
    JSValueRef r = JSObjectCallAsFunction(ctx, (JSObjectRef)fn, this_obj, n, args, exc);
    return *exc ? NULL : r;
}

JSValueRef bm_js_invoke(JSValueRef obj, JSStringRef key, size_t n, const JSValueRef *args, JSValueRef *exc) {
    JSValueRef fn = bm_js_get(obj, key, exc);
    if (!fn) return NULL;
    JSContextRef ctx = bm_js_ctx;
    if (!JSValueIsObject(ctx, fn) || !JSObjectIsFunction(ctx, (JSObjectRef)fn)) {
        char msg[200];
        bm_str k = bm_js_string_to_str(key);
        snprintf(msg, sizeof msg, "%.150s is not a function", k.p->data);
        bm_str_release(k);
        return bm_js_type_error(ctx, msg, exc);
    }
    JSValueRef r = JSObjectCallAsFunction(ctx, (JSObjectRef)fn, JSValueToObject(ctx, obj, NULL), n, args, exc);
    return *exc ? NULL : r;
}

JSValueRef bm_js_new(JSValueRef fn, size_t n, const JSValueRef *args, JSValueRef *exc) {
    JSContextRef ctx = bm_js();
    if (!JSValueIsObject(ctx, fn) || !JSObjectIsConstructor(ctx, (JSObjectRef)fn)) return bm_js_type_error(ctx, "not a constructor", exc);
    JSObjectRef r = JSObjectCallAsConstructor(ctx, (JSObjectRef)fn, n, args, exc);
    return *exc ? NULL : r;
}

JSValueRef bm_js_import(const char *spec, JSValueRef *exc) {
    JSContextRef ctx = bm_js();
    JSValueRef arg = bm_js_str(spec, strlen(spec));
    JSValueRef r = JSObjectCallAsFunction(ctx, bm_js_npm, NULL, 1, &arg, exc);
    return *exc ? NULL : r;
}

bm_str bm_js_error_text(JSValueRef exc) {
    JSContextRef ctx = bm_js_ctx;
    bm_sb sb = {0};
    bm_str s = bm_js_to_str(exc);
    bm_sb_push_str(&sb, s);
    bm_str_release(s);
    if (JSValueIsObject(ctx, exc)) {
        static JSStringRef kstack;
        JSValueRef st = JSObjectGetProperty(ctx, (JSObjectRef)exc, bm_js_key(&kstack, "stack"), NULL);
        if (st && JSValueIsString(ctx, st)) {
            bm_str t = bm_js_to_str(st);
            /* JavaScriptCore's frames are "fn@url:line:col" lines */
            const char *p = t.p->data;
            while (*p) {
                const char *e = strchr(p, '\n');
                size_t n = e ? (size_t)(e - p) : strlen(p);
                if (n) {
                    bm_sb_push(&sb, "\n    at ", 8);
                    bm_sb_push(&sb, p, n);
                }
                p += n + (e ? 1 : 0);
            }
            bm_str_release(t);
        }
    }
    return bm_str_from_sb(&sb);
}

/* ------------------------------------------------------------------ bytes */

JSObjectRef bm_js_bytes(void *ptr, size_t len, void (*dealloc)(void *bytes, void *ctx), void *dctx) {
    return JSObjectMakeTypedArrayWithBytesNoCopy(bm_js(), kJSTypedArrayTypeUint8Array, ptr, len, dealloc, dctx, NULL);
}

static void bm_js_free_bytes(void *bytes, void *ctx) { (void)ctx; free(bytes); }

JSObjectRef bm_js_bytes_copy(const void *ptr, size_t len) {
    void *copy = malloc(len ? len : 1);
    if (!copy) bm_trap("out of memory", "js");
    memcpy(copy, ptr, len);
    return bm_js_bytes(copy, len, bm_js_free_bytes, NULL);
}

bool bm_js_bytes_view(JSValueRef v, uint8_t **ptr, size_t *len) {
    JSContextRef ctx = bm_js();
    if (!JSValueIsObject(ctx, v)) return false;
    JSObjectRef o = (JSObjectRef)v;
    JSTypedArrayType t = JSValueGetTypedArrayType(ctx, v, NULL);
    if (t == kJSTypedArrayTypeNone) return false;
    if (t == kJSTypedArrayTypeArrayBuffer) {
        *ptr = JSObjectGetArrayBufferBytesPtr(ctx, o, NULL);
        *len = JSObjectGetArrayBufferByteLength(ctx, o, NULL);
        return true;
    }
    /* JSObjectGetTypedArrayBytesPtr points at the start of the backing ArrayBuffer: add the
     * view's byte offset (a Buffer is usually a slice of a shared pool) */
    *ptr = (uint8_t *)JSObjectGetTypedArrayBytesPtr(ctx, o, NULL) + JSObjectGetTypedArrayByteOffset(ctx, o, NULL);
    *len = JSObjectGetTypedArrayByteLength(ctx, o, NULL);
    return true;
}

/* ------------------------------------------------------------------ promises */

struct bm_js_deferred {
    JSObjectRef resolve, reject;
};

JSObjectRef bm_js_deferred_new(bm_js_deferred **out) {
    JSContextRef ctx = bm_js();
    bm_js_deferred *d = bm_alloc(sizeof *d);
    JSObjectRef p = JSObjectMakeDeferredPromise(ctx, &d->resolve, &d->reject, NULL);
    JSValueProtect(ctx, d->resolve);
    JSValueProtect(ctx, d->reject);
    *out = d;
    return p;
}

void bm_js_settle(bm_js_deferred *d, JSValueRef value, bool ok) {
    JSContextRef ctx = bm_js_ctx;
    JSObjectCallAsFunction(ctx, ok ? d->resolve : d->reject, NULL, 1, &value, NULL);
    JSValueUnprotect(ctx, d->resolve);
    JSValueUnprotect(ctx, d->reject);
    bm_free(d);
}

/* bm_js_await: the two reactions passed to `then` share one record. */
typedef struct {
    bm_promise *p;      /* settled by the first reaction to run (held) */
    int refs;           /* reactions not yet finalized */
} bm_js_waiter;

static JSClassRef bm_js_fulfill_class, bm_js_reject_class;

static void bm_js_waiter_settle(JSContextRef ctx, JSObjectRef f, size_t n, const JSValueRef a[], bool ok) {
    bm_js_waiter *w = JSObjectGetPrivate(f);
    if (!w || !w->p) return;
    JSValueRef v = n > 0 ? a[0] : JSValueMakeUndefined(ctx);
    bm_promise *p = w->p;
    w->p = NULL;
    if (ok) {
        bm_promise_resolve(p, &v);
    } else {
        if (!bm_js_make_error) bm_js_fatal("uncaught ", v);
        bm_promise_reject(p, bm_js_make_error(v));
    }
    bm_promise_release(p);
}
static JSValueRef bm_js_fulfill_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)self; (void)exc;
    bm_js_waiter_settle(ctx, f, n, a, true);
    return JSValueMakeUndefined(ctx);
}
static JSValueRef bm_js_reject_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)self; (void)exc;
    bm_js_waiter_settle(ctx, f, n, a, false);
    return JSValueMakeUndefined(ctx);
}
static void bm_js_waiter_finalize(JSObjectRef f) {
    bm_js_waiter *w = JSObjectGetPrivate(f);
    if (w && --w->refs == 0) {
        if (w->p) bm_promise_release(w->p);
        free(w);
    }
}

bm_promise *bm_js_await(JSValueRef v) {
    JSContextRef ctx = bm_js();
    bm_promise *p = bm_promise_new(&bm_type_js);
    static JSStringRef kthen;
    JSValueRef then = JSValueIsObject(ctx, v) ? JSObjectGetProperty(ctx, (JSObjectRef)v, bm_js_key(&kthen, "then"), NULL) : NULL;
    if (!then || !JSValueIsObject(ctx, then) || !JSObjectIsFunction(ctx, (JSObjectRef)then)) {
        bm_promise_resolve(p, &v);
        return p;
    }
    if (!bm_js_fulfill_class) {
        JSClassDefinition d = kJSClassDefinitionEmpty;
        d.callAsFunction = bm_js_fulfill_fn;
        d.finalize = bm_js_waiter_finalize;
        bm_js_fulfill_class = JSClassCreate(&d);
        d.callAsFunction = bm_js_reject_fn;
        bm_js_reject_class = JSClassCreate(&d);
    }
    bm_js_waiter *w = malloc(sizeof *w);
    if (!w) bm_trap("out of memory", "js");
    w->p = p;
    w->refs = 2;
    bm_promise_retain(p);
    JSValueRef fns[2] = {JSObjectMake(ctx, bm_js_fulfill_class, w), JSObjectMake(ctx, bm_js_reject_class, w)};
    JSValueRef exc = NULL;
    JSObjectCallAsFunction(ctx, (JSObjectRef)then, (JSObjectRef)v, 2, fns, &exc);
    if (exc && w->p) {
        /* `then` threw: the await rejects with that */
        bm_js_waiter_settle(ctx, (JSObjectRef)fns[1], 1, &exc, false);
    }
    return p;
}

/* ------------------------------------------------------------------ for generated code */

void bm_js_error_parts(JSValueRef exc, bm_str *name, bm_str *message) {
    JSContextRef ctx = bm_js();
    if (JSValueIsObject(ctx, exc)) {
        static JSStringRef kname, kmessage;
        JSValueRef n = JSObjectGetProperty(ctx, (JSObjectRef)exc, bm_js_key(&kname, "name"), NULL);
        JSValueRef m = JSObjectGetProperty(ctx, (JSObjectRef)exc, bm_js_key(&kmessage, "message"), NULL);
        if (n && JSValueIsString(ctx, n) && m && !JSValueIsUndefined(ctx, m)) {
            *name = bm_js_to_str(n);
            *message = bm_js_to_str(m);
            return;
        }
    }
    *name = bm_str_from("JsError", 7);
    *message = bm_js_to_str(exc);
}

static _Noreturn void bm_js_trap_exc(JSValueRef exc, const char *loc) {
    bm_str text = bm_js_error_text(exc);
    bm_sb sb = {0};
    bm_sb_push_cstr(&sb, "uncaught JavaScript exception: ");
    bm_sb_push_str(&sb, text);
    bm_sb_push_char(&sb, 0);
    bm_trap(sb.data, loc);
}

JSValueRef bm_js_import_as(const char *spec, int kind, const char *loc) {
    JSContextRef ctx = bm_js();
    JSValueRef exc = NULL;
    JSValueRef m = bm_js_import(spec, &exc);
    if (!m) bm_js_trap_exc(exc, loc);
    if (kind == 0) {
        /* as in Node.js: a CommonJS module's namespace has `default` (module.exports) and its keys */
        static JSStringRef kns;
        JSValueRef ns = JSObjectGetProperty(ctx, JSContextGetGlobalObject(ctx), bm_js_key(&kns, "__barm_ns"), NULL);
        JSValueRef r = JSObjectCallAsFunction(ctx, (JSObjectRef)ns, NULL, 1, &m, &exc);
        if (!r) bm_js_trap_exc(exc, loc);
        return r;
    }
    if (kind == 1 && JSValueIsObject(ctx, m)) {
        /* an ES module's default export; a CommonJS module's is module.exports */
        static JSStringRef kesm, kdefault;
        JSValueRef esm = JSObjectGetProperty(ctx, (JSObjectRef)m, bm_js_key(&kesm, "__esModule"), NULL);
        if (esm && JSValueToBoolean(ctx, esm)) {
            JSValueRef r = JSObjectGetProperty(ctx, (JSObjectRef)m, bm_js_key(&kdefault, "default"), &exc);
            if (!r || exc) bm_js_trap_exc(exc, loc);
            return r;
        }
    }
    return m;
}

JSValueRef bm_js_get_or_trap(JSValueRef obj, bm_js_name *key, const char *loc) {
    JSValueRef exc = NULL;
    JSValueRef r = bm_js_name_get(obj, key, &exc);
    if (!r) bm_js_trap_exc(exc, loc);
    return r;
}

JSValueRef bm_js_at_or_trap(JSValueRef obj, double i, const char *loc) {
    JSContextRef ctx = bm_js();
    JSValueRef exc = NULL;
    JSObjectRef o = JSValueToObject(ctx, obj, &exc);
    if (!o) bm_js_trap_exc(exc, loc);
    JSValueRef r;
    if (i >= 0 && i < 4294967295.0 && i == (double)(uint32_t)i) {
        r = JSObjectGetPropertyAtIndex(ctx, o, (unsigned)i, &exc);
    } else {
        r = JSObjectGetPropertyForKey(ctx, o, JSValueMakeNumber(ctx, i), &exc);
    }
    if (exc) bm_js_trap_exc(exc, loc);
    return r;
}

JSValueRef bm_js_key_or_trap(JSValueRef obj, bm_str key, const char *loc) {
    JSContextRef ctx = bm_js();
    JSValueRef exc = NULL;
    JSObjectRef o = JSValueToObject(ctx, obj, &exc);
    if (!o) bm_js_trap_exc(exc, loc);
    JSValueRef r = JSObjectGetPropertyForKey(ctx, o, bm_js_from_str(key), &exc);
    if (exc) bm_js_trap_exc(exc, loc);
    return r;
}

static JSStringRef bm_js_name_str(bm_js_name *k) {
    if (!k->str) k->str = JSStringCreateWithUTF8CString(k->text);
    return k->str;
}

void bm_js_put_or_trap(JSValueRef obj, bm_js_name *key, JSValueRef value, const char *loc) {
    JSValueRef exc = NULL;
    if (!bm_js_set(obj, bm_js_name_str(key), value, &exc)) bm_js_trap_exc(exc, loc);
}

_Noreturn void bm_js_type_trap(JSValueRef v, const char *want, const char *loc) {
    bm_sb sb = {0};
    bm_sb_push_cstr(&sb, "a JavaScript ");
    bm_sb_push_cstr(&sb, JSValueIsNull(bm_js(), v) ? "null" : bm_js_typeof(v));
    bm_sb_push_cstr(&sb, " isn't a `");
    bm_sb_push_cstr(&sb, want);
    bm_sb_push_cstr(&sb, "`");
    bm_sb_push_char(&sb, 0);
    bm_trap(sb.data, loc);
}

bool bm_js_truthy_slow(JSValueRef v) { return JSValueToBoolean(bm_js(), v); }
bool bm_js_is_array(JSValueRef v) { return JSValueIsArray(bm_js(), v); }

uint32_t bm_js_length(JSValueRef v) {
    JSContextRef ctx = bm_js();
    static JSStringRef klength;
    if (!JSValueIsObject(ctx, v)) return 0;
    JSValueRef n = JSObjectGetProperty(ctx, (JSObjectRef)v, bm_js_key(&klength, "length"), NULL);
    double d = n ? JSValueToNumber(ctx, n, NULL) : 0;
    return d >= 0 && d < 4294967296.0 ? (uint32_t)d : 0;
}

JSValueRef bm_js_array(size_t n, const JSValueRef *items) { return JSObjectMakeArray(bm_js(), n, items, NULL); }
JSValueRef bm_js_object(void) { return JSObjectMake(bm_js(), NULL, NULL); }

JSValueRef bm_js_lit(JSValueRef *slot, const char *s, size_t n) {
    if (!*slot) *slot = bm_js_retain(bm_js_str(s, n));
    return *slot;
}

/* Barm closures as JavaScript functions: the closure and its trampoline as private data.
 * Environments of collected functions are released later, outside the collector. */
typedef struct { bm_fn fn; bm_js_tramp tramp; } bm_js_closure;
static JSClassRef bm_js_closure_class;
static bm_env **bm_js_dead_envs;
static size_t bm_js_ndead, bm_js_capdead;

static void bm_js_release_dead(void) {
    while (bm_js_ndead) bm_env_release(bm_js_dead_envs[--bm_js_ndead]);
}

static JSValueRef bm_js_closure_call(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)ctx; (void)self; (void)exc;
    bm_js_closure *c = JSObjectGetPrivate(f);
    return c->tramp(&c->fn, n, a);
}

static void bm_js_closure_finalize(JSObjectRef f) {
    bm_js_closure *c = JSObjectGetPrivate(f);
    if (!c) return;
    if (c->fn.env) {
        if (bm_js_ndead == bm_js_capdead) {
            bm_js_capdead = bm_js_capdead ? bm_js_capdead * 2 : 16;
            bm_js_dead_envs = realloc(bm_js_dead_envs, bm_js_capdead * sizeof *bm_js_dead_envs);
            if (!bm_js_dead_envs) abort();
        }
        bm_js_dead_envs[bm_js_ndead++] = c->fn.env;
    }
    free(c);
}

JSValueRef bm_js_function(bm_fn fn, bm_js_tramp tramp) {
    JSContextRef ctx = bm_js();
    bm_js_release_dead();
    if (!bm_js_closure_class) {
        JSClassDefinition d = kJSClassDefinitionEmpty;
        d.className = "Function";
        d.callAsFunction = bm_js_closure_call;
        d.finalize = bm_js_closure_finalize;
        bm_js_closure_class = JSClassCreate(&d);
    }
    bm_js_closure *c = malloc(sizeof *c);
    if (!c) abort();
    c->fn = fn;
    c->tramp = tramp;
    bm_env_retain(fn.env);
    return JSObjectMake(ctx, bm_js_closure_class, c);
}

/* ------------------------------------------------------------------ roots
 *
 * Cells Barm holds, with counts: open addressing on the key (the cell pointer). The keys live in
 * the program's main frame (bm_js_roots_init), where the collector's conservative stack scan sees
 * them; the counts on the heap. Past 3/4 full, new cells go to JSValueProtect instead (a release
 * not found here is the protect's). Tombstones are swept out when they pile up. */

#define BM_JS_TOMB ((JSValueRef)(uintptr_t)8)   /* not a cell pointer (cells are 16-byte aligned) */
static JSValueRef *bm_js_keys;
static uint32_t *bm_js_counts;
static uint32_t bm_js_cap, bm_js_used, bm_js_live;

void bm_js_roots_init(JSValueRef *keys, uint32_t cap) {
    memset(keys, 0, (size_t)cap * sizeof *keys);
    bm_js_counts = calloc(cap, sizeof *bm_js_counts);
    if (!bm_js_counts) return;
    bm_js_keys = keys;
    bm_js_cap = cap;
}

static uint32_t bm_js_slot_of(JSValueRef v) {
    uint64_t h = (uint64_t)(uintptr_t)v >> 4;
    h *= 0x9e3779b97f4a7c15ull;
    return (uint32_t)(h >> 32) & (bm_js_cap - 1);
}

static void bm_js_sweep(void) {
    /* rebuild without tombstones (rare: only once they've piled up) */
    uint32_t n = bm_js_live, k = 0;
    JSValueRef *vs = malloc((size_t)(n ? n : 1) * sizeof *vs);
    uint32_t *cs = malloc((size_t)(n ? n : 1) * sizeof *cs);
    if (!vs || !cs) abort();
    for (uint32_t i = 0; i < bm_js_cap; i++) {
        if (bm_js_keys[i] && bm_js_keys[i] != BM_JS_TOMB) { vs[k] = bm_js_keys[i]; cs[k++] = bm_js_counts[i]; }
    }
    memset(bm_js_keys, 0, (size_t)bm_js_cap * sizeof *bm_js_keys);
    for (uint32_t j = 0; j < k; j++) {
        uint32_t i = bm_js_slot_of(vs[j]);
        while (bm_js_keys[i]) i = (i + 1) & (bm_js_cap - 1);
        bm_js_keys[i] = vs[j];
        bm_js_counts[i] = cs[j];
    }
    bm_js_used = k;
    free(vs);
    free(cs);
}

void bm_js_root_add(JSValueRef v) {
    if (bm_js_cap) {
        uint32_t i = bm_js_slot_of(v), tomb = UINT32_MAX;
        for (;;) {
            JSValueRef k = bm_js_keys[i];
            if (k == v) { bm_js_counts[i]++; return; }
            if (!k) break;
            if (k == BM_JS_TOMB && tomb == UINT32_MAX) tomb = i;
            i = (i + 1) & (bm_js_cap - 1);
        }
        if (tomb != UINT32_MAX) {
            bm_js_keys[tomb] = v;
            bm_js_counts[tomb] = 1;
            bm_js_live++;
            return;
        }
        if (bm_js_used + 1 <= bm_js_cap / 4 * 3) {
            bm_js_keys[i] = v;
            bm_js_counts[i] = 1;
            bm_js_used++;
            bm_js_live++;
            return;
        }
        if (bm_js_live < bm_js_cap / 2) {
            bm_js_sweep();
            bm_js_root_add(v);
            return;
        }
    }
    JSValueProtect(bm_js(), v);
}

void bm_js_root_remove(JSValueRef v) {
    if (bm_js_cap) {
        uint32_t i = bm_js_slot_of(v);
        for (JSValueRef k; (k = bm_js_keys[i]); i = (i + 1) & (bm_js_cap - 1)) {
            if (k == v) {
                if (--bm_js_counts[i] == 0) {
                    bm_js_keys[i] = BM_JS_TOMB;
                    bm_js_live--;
                }
                return;
            }
        }
    }
    if (bm_js_ctx) JSValueUnprotect(bm_js_ctx, v);
}

/* ------------------------------------------------------------------ names and shapes */

static bool bm_js_is_ident(const char *s) {
    if (!*s || (*s >= '0' && *s <= '9')) return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$')) return false;
    }
    return true;
}

/* `.name` or `["name"]` (JSON-quoted) */
static void bm_js_access(bm_sb *sb, const char *name) {
    if (bm_js_is_ident(name)) {
        bm_sb_push_char(sb, '.');
        bm_sb_push_cstr(sb, name);
        return;
    }
    bm_sb_push_char(sb, '[');
    bm_json_quote(sb, bm_str_from(name, strlen(name)));
    bm_sb_push_char(sb, ']');
}

/* A function compiled from `body` with parameters o, a0, a1, ... (n of the a's). */
static JSObjectRef bm_js_make_fn(const char *params_prefix, uint32_t n, bm_sb *body) {
    JSContextRef ctx = bm_js();
    JSStringRef *names = bm_alloc((size_t)(n + 1) * sizeof *names);
    uint32_t np = 0;
    if (params_prefix) names[np++] = JSStringCreateWithUTF8CString(params_prefix);
    for (uint32_t i = 0; i < n; i++) {
        char b[8];
        snprintf(b, sizeof b, "a%u", i);
        names[np++] = JSStringCreateWithUTF8CString(b);
    }
    bm_sb_push_char(body, 0);
    JSStringRef src = JSStringCreateWithUTF8CString(body->data);
    JSObjectRef f = JSObjectMakeFunction(ctx, NULL, np, names, src, NULL, 1, NULL);
    JSStringRelease(src);
    for (uint32_t i = 0; i < np; i++) JSStringRelease(names[i]);
    bm_free(names);
    bm_sb_free(body);
    if (f) JSValueProtect(ctx, f);
    return f;
}

JSValueRef bm_js_name_get(JSValueRef obj, bm_js_name *k, JSValueRef *exc) {
    JSContextRef ctx = bm_js();
    if (!k->get) {
        bm_sb b = {0};
        bm_sb_push_cstr(&b, "return o");
        bm_js_access(&b, k->text);
        k->get = bm_js_make_fn("o", 0, &b);
        if (!k->get) return bm_js_get(obj, bm_js_name_str(k), exc);
    }
    JSValueRef r = JSObjectCallAsFunction(ctx, k->get, NULL, 1, &obj, exc);
    return *exc ? NULL : r;
}

JSValueRef bm_js_name_call(JSValueRef obj, bm_js_name *k, size_t n, const JSValueRef *args, JSValueRef *exc) {
    if (n >= 5) return bm_js_invoke(obj, bm_js_name_str(k), n, args, exc);
    JSContextRef ctx = bm_js();
    if (!k->call[n]) {
        bm_sb b = {0};
        bm_sb_push_cstr(&b, "return o");
        bm_js_access(&b, k->text);
        bm_sb_push_char(&b, '(');
        for (size_t i = 0; i < n; i++) {
            char a[16];
            snprintf(a, sizeof a, "%sa%zu", i ? ", " : "", i);
            bm_sb_push_cstr(&b, a);
        }
        bm_sb_push_char(&b, ')');
        k->call[n] = bm_js_make_fn("o", (uint32_t)n, &b);
        if (!k->call[n]) return bm_js_invoke(obj, bm_js_name_str(k), n, args, exc);
    }
    JSValueRef all[5];
    all[0] = obj;
    for (size_t i = 0; i < n; i++) all[i + 1] = args[i];
    JSValueRef r = JSObjectCallAsFunction(ctx, k->call[n], NULL, n + 1, all, exc);
    return *exc ? NULL : r;
}

JSValueRef bm_js_shape_make(bm_js_shape *s, const JSValueRef *values) {
    JSContextRef ctx = bm_js();
    if (!s->make) {
        bm_sb b = {0};
        bm_sb_push_cstr(&b, "var o = {");
        bool first = true;
        for (uint32_t i = 0; i < s->n; i++) {
            if (s->optional[i]) continue;
            char a[16];
            snprintf(a, sizeof a, "a%u", i);
            if (!first) bm_sb_push_cstr(&b, ", ");
            first = false;
            bm_json_quote(&b, bm_str_from(s->keys[i], strlen(s->keys[i])));
            bm_sb_push_cstr(&b, ": ");
            bm_sb_push_cstr(&b, a);
        }
        bm_sb_push_cstr(&b, "};");
        for (uint32_t i = 0; i < s->n; i++) {
            if (!s->optional[i]) continue;
            char a[64];
            snprintf(a, sizeof a, " if (a%u !== undefined) o", i);
            bm_sb_push_cstr(&b, a);
            bm_js_access(&b, s->keys[i]);
            snprintf(a, sizeof a, " = a%u;", i);
            bm_sb_push_cstr(&b, a);
        }
        bm_sb_push_cstr(&b, " return o;");
        s->make = bm_js_make_fn(NULL, s->n, &b);
    }
    return JSObjectCallAsFunction(ctx, s->make, NULL, s->n, values, NULL);
}

JSValueRef bm_js_thunk(JSObjectRef *slot, const char *body, size_t n, const JSValueRef *args, JSValueRef *exc) {
    JSContextRef ctx = bm_js();
    if (!*slot) {
        bm_sb b = {0};
        bm_sb_push_cstr(&b, body);
        *slot = bm_js_make_fn(NULL, (uint32_t)n, &b);
        if (!*slot) bm_trap("internal error: a fused JavaScript expression didn't compile", body);
    }
    JSValueRef r = JSObjectCallAsFunction(ctx, *slot, NULL, n, args, exc);
    return *exc ? NULL : r;
}

_Noreturn void bm_js_throw_trap(JSValueRef exc, const char *loc) { bm_js_trap_exc(exc, loc); }

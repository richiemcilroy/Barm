/* npm-corpus-runner: runs a script against an npm bundle on Barm's runtime (runtime/js.c,
 * node.c and barm.c, as a Barm program would), the bundle read from a file rather than embedded.
 *   npm-corpus-runner <bundle.blob> <script.js>
 * The script sees __barm_npm(spec) and __out(text) (a line on stdout). Built by
 * scripts/npm-corpus.py. */
#include "js.h"
#include <JavaScriptCore/JavaScriptCore.h>
#include <stdio.h>
#include <stdlib.h>

/* the blob js.c reads (`bm_js_blob`), filled from the file */
unsigned char bm_js_blob_storage[128 << 20] __asm__("_bm_js_blob") __attribute__((aligned(16)));

static void *make_error(JSValueRef e) { (void)e; return NULL; }

static JSValueRef out_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    if (n) {
        bm_str s = bm_js_to_str(a[0]);
        fwrite(s.p->data, 1, (size_t)s.p->len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return JSValueMakeUndefined(ctx);
}

int main(int argc, char **argv) {
    bm_init(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: npm-corpus-runner <bundle.blob> <script.js>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    size_t n = fread(bm_js_blob_storage, 1, sizeof bm_js_blob_storage - 1, f);
    fclose(f);
    bm_js_blob_storage[n] = 0;
    JSValueRef roots[4096];
    bm_js_roots_init(roots, 4096);
    bm_js_make_error = make_error;
    static char src[1 << 20];
    f = fopen(argv[2], "rb");
    if (!f) return 2;
    size_t m = fread(src, 1, sizeof src - 1, f);
    fclose(f);
    src[m] = 0;
    JSContextRef ctx = bm_js();
    bm_js_def(ctx, JSContextGetGlobalObject(ctx), "__out", out_fn);
    JSStringRef s = JSStringCreateWithUTF8CString(src);
    JSValueRef exc = NULL;
    JSEvaluateScript(ctx, s, NULL, NULL, 1, &exc);
    if (exc) {
        bm_str t = bm_js_error_text(exc);
        printf("THROW %s\n", t.p->data);
    }
    bm_async_run();
    return 0;
}

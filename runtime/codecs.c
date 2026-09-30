/* codecs.c — brotli and zstd bodies for fetch(), on the vendored decoders (vendor/brotli,
 * vendor/zstd). Compiled into the same archive as tls.c; bm_tls_install() points the runtime's
 * bm_decode_brotli/bm_decode_zstd here, so only programs that fetch link them. (gzip and deflate
 * are decoded by the runtime itself.) */

#include "barm.h"

#include <brotli/decode.h>
#include <zstd.h>

/* Decoded bodies become strings: < 2 GiB. */
#define BM_DECODED_MAX ((size_t)INT32_MAX)

bool bm_codec_brotli(const uint8_t *p, size_t n, bm_sb *out) {
    BrotliDecoderState *s = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!s) return false;
    size_t avail_in = n;
    const uint8_t *next_in = p;
    bm_sb_grow(out, out->len + (n < 1024 ? 4096 : n * 4));
    bool ok = false;
    for (;;) {
        size_t avail_out = out->cap - out->len;
        uint8_t *next_out = (uint8_t *)out->data + out->len;
        BrotliDecoderResult r = BrotliDecoderDecompressStream(s, &avail_in, &next_in, &avail_out, &next_out, NULL);
        out->len = (size_t)((char *)next_out - out->data);
        if (r == BROTLI_DECODER_RESULT_SUCCESS) { ok = true; break; }
        if (r != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT || out->len >= BM_DECODED_MAX) break; /* corrupt or cut short */
        bm_sb_grow(out, out->cap * 2);
    }
    BrotliDecoderDestroyInstance(s);
    return ok;
}

bool bm_codec_zstd(const uint8_t *p, size_t n, bm_sb *out) {
    static ZSTD_DCtx *dctx;
    if (!dctx && !(dctx = ZSTD_createDCtx())) return false;
    ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
    /* a single frame usually says its size: reserve it */
    unsigned long long known = ZSTD_getFrameContentSize(p, n);
    size_t first = known != ZSTD_CONTENTSIZE_UNKNOWN && known != ZSTD_CONTENTSIZE_ERROR && known < BM_DECODED_MAX ? (size_t)known + 1 : (n < 1024 ? 4096 : n * 4);
    bm_sb_grow(out, out->len + first);
    ZSTD_inBuffer in = { p, n, 0 };
    size_t hint = 1;
    while (in.pos < in.size || hint != 0) {
        if (out->cap == out->len) {
            if (out->len >= BM_DECODED_MAX) return false;
            bm_sb_grow(out, out->cap * 2);
        }
        ZSTD_outBuffer o = { out->data + out->len, out->cap - out->len, 0 };
        size_t before = in.pos;
        hint = ZSTD_decompressStream(dctx, &o, &in);
        if (ZSTD_isError(hint)) return false;
        out->len += o.pos;
        if (in.pos == in.size && hint != 0 && o.pos == 0 && in.pos == before) return false; /* cut short */
    }
    return true;
}

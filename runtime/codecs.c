/* codecs.c — decoders for fetch() bodies: gzip and deflate (vendor/zlib, vendor/libdeflate),
 * brotli (vendor/brotli) and zstd (vendor/zstd). Compiled into the same archive as tls.c;
 * bm_tls_install() points the runtime's bm_codec at them, so only programs that fetch link them.
 *
 * A body read in chunks (res.body) is decoded as it arrives, by the streaming decoders. A body
 * read whole (text() and the like) is decoded at once when it's all in: for gzip and deflate by
 * libdeflate, which is twice as fast as zlib but can't stream. */

#include "barm.h"

#include <brotli/decode.h>
#include <libdeflate.h>
#include <zlib.h>
#include <zstd.h>

/* Decoded bodies become strings: < 2 GiB. */
#define BM_DECODED_MAX ((size_t)INT32_MAX)

enum { BM_ENC_GZIP = 1, BM_ENC_DEFLATE, BM_ENC_BR, BM_ENC_ZSTD };

struct bm_decoder {
    int enc;
    bool ended;          /* at a clean end (for gzip and zstd, another member or frame may follow) */
    bool started;        /* zlib: inflateInit2 done (deflate waits for 2 bytes to pick its wrapper) */
    bool rest_ignored;   /* gzip, deflate, brotli: what follows the end isn't decoded */
    bool mid_frame;      /* zstd: a frame has begun */
    z_stream z;
    BrotliDecoderState *br;
    ZSTD_DCtx *zstd;
};

static bm_decoder *bm_codec_open(int enc) {
    bm_decoder *d = bm_alloc(sizeof *d);
    memset(d, 0, sizeof *d);
    d->enc = enc;
    if (enc == BM_ENC_GZIP) {
        if (inflateInit2(&d->z, 16 + MAX_WBITS) != Z_OK) { bm_free(d); return NULL; }
        d->started = true;
    } else if (enc == BM_ENC_BR) {
        if (!(d->br = BrotliDecoderCreateInstance(NULL, NULL, NULL))) { bm_free(d); return NULL; }
    } else if (enc == BM_ENC_ZSTD) {
        if (!(d->zstd = ZSTD_createDCtx())) { bm_free(d); return NULL; }
    } else if (enc != BM_ENC_DEFLATE) {
        bm_free(d);
        return NULL;
    }
    return d;
}

static void bm_codec_close(bm_decoder *d) {
    if (!d) return;
    if (d->started) inflateEnd(&d->z);
    if (d->br) BrotliDecoderDestroyInstance(d->br);
    if (d->zstd) ZSTD_freeDCtx(d->zstd);
    bm_free(d);
}

/* Room for more output, but no more than `limit` in all (0: no limit). The buffer only ever
 * doubles: every body of a size goes through the same buffer sizes, so the allocator reuses the
 * blocks the last one freed (sizes that follow the input's chunks would each leave a freed
 * block of their own in macOS's large-block cache: hundreds of MB, over many downloads). */
static size_t bm_codec_room(bm_sb *out, size_t start, size_t limit) {
    size_t max = limit ? start + limit : BM_DECODED_MAX;
    if (out->len >= max) return 0;
    if (out->cap - out->len < 32768) bm_sb_grow(out, out->cap < 65536 ? 65536 : out->cap * 2);
    size_t room = out->cap - out->len;
    return room > max - out->len ? max - out->len : room;
}

/* gzip and deflate through zlib. */
static int bm_codec_zlib(bm_decoder *d, const uint8_t *in, size_t n, size_t *used, bm_sb *out, size_t limit) {
    size_t start = out->len;
    while (*used < n) {
        if (d->rest_ignored) { *used = n; break; }
        if (!d->started) {
            /* "deflate" is zlib-wrapped in the standard, but some servers send it raw: accept both */
            if (n - *used < 2) break;
            const uint8_t *p = in + *used;
            bool zlib = (p[0] & 0x0f) == 8 && (p[0] >> 4) <= 7 && ((p[0] << 8) | p[1]) % 31 == 0 && !(p[1] & 0x20);
            if (inflateInit2(&d->z, zlib ? MAX_WBITS : -MAX_WBITS) != Z_OK) return -1;
            d->started = true;
        }
        if (d->ended) {
            /* gzip: another member may follow (anything else after the end is ignored, as by
             * browsers) */
            if (d->enc != BM_ENC_GZIP) { d->rest_ignored = true; continue; }
            if (n - *used < 2) break;
            if (in[*used] != 0x1f || in[*used + 1] != 0x8b) { d->rest_ignored = true; continue; }
            if (inflateReset(&d->z) != Z_OK) return -1;
            d->ended = false;
        }
        size_t room = bm_codec_room(out, start, limit);
        if (!room) break;
        size_t in_n = n - *used;
        d->z.next_in = (Bytef *)(in + *used);
        d->z.avail_in = in_n > UINT32_MAX ? UINT32_MAX : (uInt)in_n;
        d->z.next_out = (Bytef *)out->data + out->len;
        d->z.avail_out = room > UINT32_MAX ? UINT32_MAX : (uInt)room;
        uInt before_in = d->z.avail_in, before_out = d->z.avail_out;
        int rc = inflate(&d->z, Z_NO_FLUSH);
        *used += before_in - d->z.avail_in;
        out->len += before_out - d->z.avail_out;
        if (rc == Z_STREAM_END) { d->ended = true; continue; }
        if (rc == Z_BUF_ERROR) {
            if (before_in == d->z.avail_in && before_out == d->z.avail_out) break; /* needs more of both */
            continue;
        }
        if (rc != Z_OK) return -1;
    }
    return d->ended ? 1 : 0;
}

static int bm_codec_brotli(bm_decoder *d, const uint8_t *in, size_t n, size_t *used, bm_sb *out, size_t limit) {
    size_t start = out->len;
    for (;;) {
        if (d->ended) { *used = n; return 1; }
        size_t room = bm_codec_room(out, start, limit);
        if (!room) return 0;
        size_t avail_in = n - *used, avail_out = room;
        const uint8_t *next_in = in + *used;
        uint8_t *next_out = (uint8_t *)out->data + out->len;
        BrotliDecoderResult r = BrotliDecoderDecompressStream(d->br, &avail_in, &next_in, &avail_out, &next_out, NULL);
        *used = n - avail_in;
        out->len += room - avail_out;
        if (r == BROTLI_DECODER_RESULT_SUCCESS) { d->ended = true; continue; }
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) return 0;
        if (r != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) return -1;
    }
}

static int bm_codec_zstd(bm_decoder *d, const uint8_t *in, size_t n, size_t *used, bm_sb *out, size_t limit) {
    size_t start = out->len;
    while (*used < n || !d->ended) {
        /* a frame usually says its size: reserve it (a size of its own, but once) */
        if (!d->mid_frame && n - *used >= 18) {
            unsigned long long known = ZSTD_getFrameContentSize(in + *used, n - *used);
            if (known != ZSTD_CONTENTSIZE_UNKNOWN && known != ZSTD_CONTENTSIZE_ERROR && known < BM_DECODED_MAX && out->cap - out->len < known)
                bm_sb_grow(out, out->len + (size_t)known);
        }
        size_t room = bm_codec_room(out, start, limit);
        if (!room) break;
        ZSTD_inBuffer ib = { in, n, *used };
        ZSTD_outBuffer ob = { out->data + out->len, room, 0 };
        size_t hint = ZSTD_decompressStream(d->zstd, &ob, &ib);
        if (ZSTD_isError(hint)) return -1;
        bool progress = ib.pos != *used || ob.pos != 0;
        *used = ib.pos;
        out->len += ob.pos;
        d->ended = hint == 0; /* a frame is complete and flushed */
        d->mid_frame = !d->ended;
        if (!progress) break;
        if (d->ended && *used == n) break;
    }
    return d->ended ? 1 : 0;
}

/* Decodes in[*used, n) onto out, at most `limit` new bytes (0: no limit); advances *used.
 * 1: the input so far is a complete stream; 0: more is needed (input, or room); -1: corrupt. */
static int bm_codec_step(bm_decoder *d, const uint8_t *in, size_t n, size_t *used, bm_sb *out, size_t limit) {
    if (d->enc == BM_ENC_BR) return bm_codec_brotli(d, in, n, used, out, limit);
    if (d->enc == BM_ENC_ZSTD) return bm_codec_zstd(d, in, n, used, out, limit);
    return bm_codec_zlib(d, in, n, used, out, limit);
}

static uint32_t bm_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Output room for `want` more bytes: at most deflate's largest expansion (1032:1) of what's
 * left to decode, so a size the input only claims can't reserve more than it could produce. */
static bool bm_codec_reserve(bm_sb *out, size_t want, size_t in_left) {
    size_t most = in_left > BM_DECODED_MAX / 1032 ? BM_DECODED_MAX : in_left * 1032 + 1024;
    if (want > most) want = most;
    if (out->len + want > BM_DECODED_MAX) return false;
    if (out->cap - out->len < want) bm_sb_grow(out, out->len + want);
    return true;
}

/* A whole gzip or deflate body through libdeflate. */
static bool bm_codec_whole_deflate(int enc, const uint8_t *in, size_t n, bm_sb *out) {
    static struct libdeflate_decompressor *ld;
    if (!ld && !(ld = libdeflate_alloc_decompressor())) return false;
    if (enc == BM_ENC_GZIP) {
        /* member by member (anything after the last is ignored, as by browsers); a member's
         * trailer gives its size, and a one-member body's is the body's last 4 bytes */
        bool any = false;
        size_t pos = 0;
        while (n - pos >= 18 && in[pos] == 0x1f && in[pos + 1] == 0x8b) {
            size_t want = pos == 0 ? bm_le32(in + n - 4) : (n - pos) * 4;
            for (;;) {
                if (!bm_codec_reserve(out, want ? want : 1, n - pos)) return false;
                size_t in_used = 0, got = 0;
                enum libdeflate_result r = libdeflate_gzip_decompress_ex(ld, in + pos, n - pos, out->data + out->len, out->cap - out->len, &in_used, &got);
                if (r == LIBDEFLATE_SUCCESS) { out->len += got; pos += in_used; any = true; break; }
                if (r != LIBDEFLATE_INSUFFICIENT_SPACE) return false;
                want = (out->cap - out->len) * 2 + (1 << 20);
            }
        }
        return any;
    }
    /* "deflate" is zlib-wrapped in the standard, but some servers send it raw: accept both */
    bool zlib = n >= 2 && (in[0] & 0x0f) == 8 && (in[0] >> 4) <= 7 && ((in[0] << 8) | in[1]) % 31 == 0 && !(in[1] & 0x20);
    size_t want = n * 4 + (64 << 10);
    for (;;) {
        if (!bm_codec_reserve(out, want, n)) return false;
        size_t in_used = 0, got = 0;
        enum libdeflate_result r = zlib ? libdeflate_zlib_decompress_ex(ld, in, n, out->data + out->len, out->cap - out->len, &in_used, &got)
                                        : libdeflate_deflate_decompress_ex(ld, in, n, out->data + out->len, out->cap - out->len, &in_used, &got);
        if (r == LIBDEFLATE_SUCCESS) { out->len += got; return true; }
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE || out->cap - out->len >= BM_DECODED_MAX / 2) return false;
        want = (out->cap - out->len) * 2;
    }
}

/* A whole body at once onto out: false if it's corrupt or cut short. */
static bool bm_codec_whole(int enc, const uint8_t *in, size_t n, bm_sb *out) {
    if (enc == BM_ENC_GZIP || enc == BM_ENC_DEFLATE) return bm_codec_whole_deflate(enc, in, n, out);
    bm_decoder *d = bm_codec_open(enc);
    if (!d) return false;
    size_t used = 0;
    int st = bm_codec_step(d, in, n, &used, out, 0);
    bm_codec_close(d);
    return st == 1;
}

const bm_codec_ops bm_codecs = { bm_codec_open, bm_codec_step, bm_codec_close, bm_codec_whole };

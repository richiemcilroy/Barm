/* compress.c — the compression streams behind npm packages' `zlib` (Node.js's src/node_zlib.cc,
 * which runtime/node.c and internal/bindings/zlib.js expose as internalBinding('zlib')): zlib's
 * deflate and inflate in their gzip, raw and auto-detecting forms, brotli and zstd, each way.
 * They answer and fail as Node.js's do: the same modes, flush values, multi-member gzip, preset
 * dictionaries, and error messages and codes. Compiled into the same archive as tls.c;
 * bm_zlib_install() points the runtime's bm_zs at them, so only programs that need them link them.
 * Plain C: no JavaScriptCore here. */

#include "barm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>

/* Node.js's node_zlib_mode */
enum { ZS_NONE, ZS_DEFLATE, ZS_INFLATE, ZS_GZIP, ZS_GUNZIP, ZS_DEFLATERAW, ZS_INFLATERAW, ZS_UNZIP, ZS_BROTLI_DECODE, ZS_BROTLI_ENCODE,
       ZS_ZSTD_COMPRESS, ZS_ZSTD_DECOMPRESS };

struct bm_zstream {
    int mode;
    int flush;
    /* zlib */
    z_stream strm;
    int err;
    int level, window_bits, mem_level, strategy;
    bool zlib_init_done;
    int gzip_id_bytes_read;
    uint8_t *dict;
    size_t dict_len;
    /* brotli */
    BrotliEncoderState *br_enc;
    BrotliDecoderState *br_dec;
    BrotliEncoderPreparedDictionary *br_dict;
    const uint8_t *next_in;
    uint8_t *next_out;
    size_t avail_in, avail_out;
    bool br_ok;                        /* the encoder's last result */
    BrotliDecoderResult br_result;
    BrotliDecoderErrorCode br_error;
    char br_error_code[64];
    /* zstd */
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
    uint64_t pledged;
    ZSTD_inBuffer zin;
    ZSTD_outBuffer zout;
    ZSTD_ErrorCode zstd_error;
};

static bool zs_zlib(const bm_zstream *z) { return z->mode >= ZS_DEFLATE && z->mode <= ZS_UNZIP; }
static bool zs_brotli(const bm_zstream *z) { return z->mode == ZS_BROTLI_DECODE || z->mode == ZS_BROTLI_ENCODE; }

static bm_zs_error zs_none(void) { return (bm_zs_error){ NULL, NULL, 0 }; }

static bm_zs_error zs_err(const char *message, const char *code, int err) { return (bm_zs_error){ message, code, err }; }

static const char *zs_zlib_strerror(int err) {
    switch (err) {
    case Z_OK: return "Z_OK";
    case Z_STREAM_END: return "Z_STREAM_END";
    case Z_NEED_DICT: return "Z_NEED_DICT";
    case Z_ERRNO: return "Z_ERRNO";
    case Z_STREAM_ERROR: return "Z_STREAM_ERROR";
    case Z_DATA_ERROR: return "Z_DATA_ERROR";
    case Z_MEM_ERROR: return "Z_MEM_ERROR";
    case Z_BUF_ERROR: return "Z_BUF_ERROR";
    case Z_VERSION_ERROR: return "Z_VERSION_ERROR";
    default: return "Z_UNKNOWN_ERROR";
    }
}

static const char *zs_zstd_strerror(ZSTD_ErrorCode e) {
    switch (e) {
#define V(c) case ZSTD_##c: return "ZSTD_" #c;
    V(error_no_error) V(error_GENERIC) V(error_prefix_unknown) V(error_version_unsupported) V(error_frameParameter_unsupported)
    V(error_frameParameter_windowTooLarge) V(error_corruption_detected) V(error_checksum_wrong) V(error_literals_headerWrong)
    V(error_dictionary_corrupted) V(error_dictionary_wrong) V(error_dictionaryCreation_failed) V(error_parameter_unsupported)
    V(error_parameter_combination_unsupported) V(error_parameter_outOfBound) V(error_tableLog_tooLarge)
    V(error_maxSymbolValue_tooLarge) V(error_maxSymbolValue_tooSmall) V(error_stabilityCondition_notRespected)
    V(error_stage_wrong) V(error_init_missing) V(error_memory_allocation) V(error_workSpace_tooSmall) V(error_dstSize_tooSmall)
    V(error_srcSize_wrong) V(error_dstBuffer_null) V(error_noForwardProgress_destFull) V(error_noForwardProgress_inputEmpty)
#undef V
    default: return "ZSTD_error_GENERIC";
    }
}

static bm_zstream *zs_open(int mode) {
    bm_zstream *z = calloc(1, sizeof *z);
    if (z) z->mode = mode;
    return z;
}

/* ---------------------------------------------------------------- zlib (ZlibContext) */

static bm_zs_error zs_zlib_message(const bm_zstream *z, const char *message) {
    return zs_err(z->strm.msg ? z->strm.msg : message, zs_zlib_strerror(z->err), z->err);
}

static bm_zs_error zs_set_dictionary(bm_zstream *z) {
    if (!z->dict_len) return zs_none();
    z->err = Z_OK;
    if (z->mode == ZS_DEFLATE || z->mode == ZS_DEFLATERAW) z->err = deflateSetDictionary(&z->strm, z->dict, (uInt)z->dict_len);
    else if (z->mode == ZS_INFLATERAW) z->err = inflateSetDictionary(&z->strm, z->dict, (uInt)z->dict_len);
    return z->err != Z_OK ? zs_zlib_message(z, "Failed to set dictionary") : zs_none();
}

/* true on the first call (which may have failed: z->err) */
static bool zs_init_zlib_now(bm_zstream *z) {
    if (z->zlib_init_done) return false;
    switch (z->mode) {
    case ZS_DEFLATE: case ZS_GZIP: case ZS_DEFLATERAW:
        z->err = deflateInit2(&z->strm, z->level, Z_DEFLATED, z->window_bits, z->mem_level, z->strategy);
        break;
    default:
        z->err = inflateInit2(&z->strm, z->window_bits);
        break;
    }
    if (z->err != Z_OK) {
        free(z->dict);
        z->dict = NULL;
        z->dict_len = 0;
        z->mode = ZS_NONE;
        return true;
    }
    zs_set_dictionary(z);
    z->zlib_init_done = true;
    return true;
}

static void zs_init_zlib(bm_zstream *z, int window_bits, int level, int mem_level, int strategy, const uint8_t *dict, size_t n) {
    z->level = level;
    z->window_bits = window_bits;
    z->mem_level = mem_level;
    z->strategy = strategy;
    z->flush = Z_NO_FLUSH;
    z->err = Z_OK;
    if (z->mode == ZS_GZIP || z->mode == ZS_GUNZIP) z->window_bits += 16;
    if (z->mode == ZS_UNZIP) z->window_bits += 32;
    if (z->mode == ZS_DEFLATERAW || z->mode == ZS_INFLATERAW) z->window_bits *= -1;
    if (n) {
        z->dict = malloc(n);
        memcpy(z->dict, dict, n);
        z->dict_len = n;
    }
}

static bm_zs_error zs_reset_zlib(bm_zstream *z) {
    if (zs_init_zlib_now(z) && z->err != Z_OK) return zs_zlib_message(z, "Failed to init stream before reset");
    z->err = Z_OK;
    switch (z->mode) {
    case ZS_DEFLATE: case ZS_DEFLATERAW: case ZS_GZIP: z->err = deflateReset(&z->strm); break;
    case ZS_INFLATE: case ZS_INFLATERAW: case ZS_GUNZIP: z->err = inflateReset(&z->strm); break;
    default: break;
    }
    if (z->err != Z_OK) return zs_zlib_message(z, "Failed to reset stream");
    return zs_set_dictionary(z);
}

static void zs_work_zlib(bm_zstream *z) {
    if (zs_init_zlib_now(z) && z->err != Z_OK) return;
    const Bytef *next = NULL;
    switch (z->mode) {
    case ZS_DEFLATE: case ZS_GZIP: case ZS_DEFLATERAW:
        z->err = deflate(&z->strm, z->flush);
        return;
    case ZS_UNZIP:
        if (z->strm.avail_in > 0) next = z->strm.next_in;
        if (z->gzip_id_bytes_read == 0) {
            if (next == NULL) goto inflate;
            if (*next == 0x1f) {
                z->gzip_id_bytes_read = 1;
                next++;
                if (z->strm.avail_in == 1) goto inflate;
            } else {
                z->mode = ZS_INFLATE;
                goto inflate;
            }
        }
        if (next == NULL) goto inflate;
        if (*next == 0x8b) {
            z->gzip_id_bytes_read = 2;
            z->mode = ZS_GUNZIP;
        } else {
            z->mode = ZS_INFLATE;
        }
        break;
    default:
        break;
    }
inflate:
    z->err = inflate(&z->strm, z->flush);
    if (z->mode != ZS_INFLATERAW && z->err == Z_NEED_DICT && z->dict_len) {
        z->err = inflateSetDictionary(&z->strm, z->dict, (uInt)z->dict_len);
        if (z->err == Z_OK) z->err = inflate(&z->strm, z->flush);
        else if (z->err == Z_DATA_ERROR) z->err = Z_NEED_DICT;
    }
    /* another gzip member, or trailing garbage (zero padding is fine) */
    while (z->strm.avail_in > 0 && z->mode == ZS_GUNZIP && z->err == Z_STREAM_END && z->strm.next_in[0] != 0) {
        zs_reset_zlib(z);
        z->err = inflate(&z->strm, z->flush);
    }
}

static bm_zs_error zs_check_zlib(const bm_zstream *z) {
    switch (z->err) {
    case Z_OK:
    case Z_BUF_ERROR:
        if (z->strm.avail_out != 0 && z->flush == Z_FINISH) return zs_zlib_message(z, "unexpected end of file");
        /* fallthrough */
    case Z_STREAM_END:
        return zs_none();
    case Z_NEED_DICT:
        return zs_zlib_message(z, z->dict_len ? "Bad dictionary" : "Missing dictionary");
    default:
        return zs_zlib_message(z, "Zlib error");
    }
}

static bm_zs_error zs_params_zlib(bm_zstream *z, int level, int strategy) {
    if (zs_init_zlib_now(z) && z->err != Z_OK) return zs_zlib_message(z, "Failed to init stream before set parameters");
    z->err = Z_OK;
    if (z->mode == ZS_DEFLATE || z->mode == ZS_DEFLATERAW) z->err = deflateParams(&z->strm, level, strategy);
    if (z->err != Z_OK && z->err != Z_BUF_ERROR) return zs_zlib_message(z, "Failed to set parameters");
    return zs_none();
}

static void zs_close_zlib(bm_zstream *z) {
    if (z->zlib_init_done) {
        if (z->mode == ZS_DEFLATE || z->mode == ZS_GZIP || z->mode == ZS_DEFLATERAW) deflateEnd(&z->strm);
        else if (z->mode != ZS_NONE) inflateEnd(&z->strm);
        z->zlib_init_done = false;
    }
    free(z->dict);
    z->dict = NULL;
    z->dict_len = 0;
}

/* ---------------------------------------------------------------- brotli */

static void zs_close_brotli(bm_zstream *z) {
    if (z->br_enc) BrotliEncoderDestroyInstance(z->br_enc);
    if (z->br_dict) BrotliEncoderDestroyPreparedDictionary(z->br_dict);
    if (z->br_dec) BrotliDecoderDestroyInstance(z->br_dec);
    z->br_enc = NULL;
    z->br_dict = NULL;
    z->br_dec = NULL;
}

static bm_zs_error zs_init_brotli_state(bm_zstream *z) {
    zs_close_brotli(z);
    z->br_ok = true;
    z->br_result = BROTLI_DECODER_RESULT_SUCCESS;
    z->br_error = BROTLI_DECODER_NO_ERROR;
    if (z->mode == ZS_BROTLI_ENCODE) {
        z->br_enc = BrotliEncoderCreateInstance(NULL, NULL, NULL);
        if (!z->br_enc) return zs_err("Could not initialize Brotli instance", "ERR_ZLIB_INITIALIZATION_FAILED", -1);
        if (z->dict_len) {
            z->br_dict = BrotliEncoderPrepareDictionary(BROTLI_SHARED_DICTIONARY_RAW, z->dict_len, z->dict, BROTLI_MAX_QUALITY, NULL, NULL, NULL);
            if (!z->br_dict) return zs_err("Failed to prepare brotli dictionary", "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
            if (!BrotliEncoderAttachPreparedDictionary(z->br_enc, z->br_dict))
                return zs_err("Failed to attach brotli dictionary", "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
        }
    } else {
        z->br_dec = BrotliDecoderCreateInstance(NULL, NULL, NULL);
        if (!z->br_dec) return zs_err("Could not initialize Brotli instance", "ERR_ZLIB_INITIALIZATION_FAILED", -1);
        if (z->dict_len && !BrotliDecoderAttachDictionary(z->br_dec, BROTLI_SHARED_DICTIONARY_RAW, z->dict_len, z->dict))
            return zs_err("Failed to attach brotli dictionary", "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
    }
    return zs_none();
}

static bm_zs_error zs_init_brotli(bm_zstream *z, const uint32_t *params, size_t n, const uint8_t *dict, size_t dict_len) {
    free(z->dict);
    z->dict = NULL;
    z->dict_len = 0;
    if (dict_len) {
        z->dict = malloc(dict_len);
        memcpy(z->dict, dict, dict_len);
        z->dict_len = dict_len;
    }
    bm_zs_error e = zs_init_brotli_state(z);
    if (e.message) return e;
    for (size_t i = 0; i < n; i++) {
        if (params[i] == UINT32_MAX) continue;
        bool ok = z->mode == ZS_BROTLI_ENCODE ? BrotliEncoderSetParameter(z->br_enc, (BrotliEncoderParameter)i, params[i])
                                               : BrotliDecoderSetParameter(z->br_dec, (BrotliDecoderParameter)i, params[i]);
        if (!ok) return zs_err("Setting parameter failed", "ERR_BROTLI_PARAM_SET_FAILED", -1);
    }
    return zs_none();
}

static void zs_work_brotli(bm_zstream *z) {
    const uint8_t *next_in = z->next_in;
    if (z->mode == ZS_BROTLI_ENCODE) {
        z->br_ok = BrotliEncoderCompressStream(z->br_enc, (BrotliEncoderOperation)z->flush, &z->avail_in, &next_in, &z->avail_out, &z->next_out, NULL);
    } else {
        z->br_result = BrotliDecoderDecompressStream(z->br_dec, &z->avail_in, &next_in, &z->avail_out, &z->next_out, NULL);
        if (z->br_result == BROTLI_DECODER_RESULT_ERROR) {
            z->br_error = BrotliDecoderGetErrorCode(z->br_dec);
            snprintf(z->br_error_code, sizeof z->br_error_code, "ERR_%s", BrotliDecoderErrorString(z->br_error));
        }
    }
    z->next_in = next_in;
}

static bm_zs_error zs_check_brotli(const bm_zstream *z) {
    if (z->mode == ZS_BROTLI_ENCODE) return z->br_ok ? zs_none() : zs_err("Compression failed", "ERR_BROTLI_COMPRESSION_FAILED", -1);
    if (z->br_error != BROTLI_DECODER_NO_ERROR) return zs_err("Decompression failed", z->br_error_code, (int)z->br_error);
    if (z->flush == BROTLI_OPERATION_FINISH && z->br_result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT)
        return zs_err("unexpected end of file", "Z_BUF_ERROR", Z_BUF_ERROR);
    return zs_none();
}

/* ---------------------------------------------------------------- zstd */

static void zs_close_zstd(bm_zstream *z) {
    ZSTD_freeCCtx(z->cctx);
    ZSTD_freeDCtx(z->dctx);
    z->cctx = NULL;
    z->dctx = NULL;
}

static bm_zs_error zs_init_zstd_state(bm_zstream *z) {
    zs_close_zstd(z);
    z->zstd_error = ZSTD_error_no_error;
    if (z->mode == ZS_ZSTD_COMPRESS) {
        z->cctx = ZSTD_createCCtx();
        if (!z->cctx) return zs_err("Could not initialize zstd instance", "ERR_ZLIB_INITIALIZATION_FAILED", -1);
        if (z->dict_len && ZSTD_isError(ZSTD_CCtx_loadDictionary(z->cctx, z->dict, z->dict_len)))
            return zs_err("Failed to load zstd dictionary", "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
        if (ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(z->cctx, z->pledged)))
            return zs_err("Could not set pledged src size", "ERR_ZLIB_INITIALIZATION_FAILED", -1);
    } else {
        z->dctx = ZSTD_createDCtx();
        if (!z->dctx) return zs_err("Could not initialize zstd instance", "ERR_ZLIB_INITIALIZATION_FAILED", -1);
        if (z->dict_len && ZSTD_isError(ZSTD_DCtx_loadDictionary(z->dctx, z->dict, z->dict_len)))
            return zs_err("Failed to load zstd dictionary", "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
    }
    return zs_none();
}

static bm_zs_error zs_init_zstd(bm_zstream *z, const uint32_t *params, size_t n, uint64_t pledged, const uint8_t *dict, size_t dict_len) {
    z->pledged = pledged;
    free(z->dict);
    z->dict = NULL;
    z->dict_len = 0;
    if (dict_len) {
        z->dict = malloc(dict_len);
        memcpy(z->dict, dict, dict_len);
        z->dict_len = dict_len;
    }
    bm_zs_error e = zs_init_zstd_state(z);
    if (e.message) return e;
    for (size_t i = 0; i < n; i++) {
        if (params[i] == UINT32_MAX) continue;
        size_t r = z->mode == ZS_ZSTD_COMPRESS ? ZSTD_CCtx_setParameter(z->cctx, (ZSTD_cParameter)i, (int)params[i])
                                                : ZSTD_DCtx_setParameter(z->dctx, (ZSTD_dParameter)i, (int)params[i]);
        if (ZSTD_isError(r)) return zs_err("Setting parameter failed", "ERR_ZSTD_PARAM_SET_FAILED", -1);
    }
    return zs_none();
}

static void zs_work_zstd(bm_zstream *z) {
    size_t r = z->mode == ZS_ZSTD_COMPRESS ? ZSTD_compressStream2(z->cctx, &z->zout, &z->zin, (ZSTD_EndDirective)z->flush)
                                            : ZSTD_decompressStream(z->dctx, &z->zout, &z->zin);
    if (ZSTD_isError(r)) z->zstd_error = ZSTD_getErrorCode(r);
}

static bm_zs_error zs_check_zstd(const bm_zstream *z) {
    if (z->zstd_error == ZSTD_error_no_error) return zs_none();
    return zs_err(ZSTD_getErrorString(z->zstd_error), zs_zstd_strerror(z->zstd_error), (int)z->zstd_error);
}

/* ---------------------------------------------------------------- the stream */

static void zs_write(bm_zstream *z, int flush, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len, uint32_t *avail_in,
                     uint32_t *avail_out) {
    z->flush = flush;
    if (zs_zlib(z) || z->mode == ZS_NONE) {
        z->strm.next_in = (Bytef *)in;
        z->strm.avail_in = in_len;
        z->strm.next_out = out;
        z->strm.avail_out = out_len;
        if (z->mode != ZS_NONE) zs_work_zlib(z);
        *avail_in = z->strm.avail_in;
        *avail_out = z->strm.avail_out;
    } else if (zs_brotli(z)) {
        z->next_in = in;
        z->avail_in = in_len;
        z->next_out = out;
        z->avail_out = out_len;
        zs_work_brotli(z);
        *avail_in = (uint32_t)z->avail_in;
        *avail_out = (uint32_t)z->avail_out;
    } else {
        z->zin = (ZSTD_inBuffer){ in, in_len, 0 };
        z->zout = (ZSTD_outBuffer){ out, out_len, 0 };
        zs_work_zstd(z);
        *avail_in = (uint32_t)(z->zin.size - z->zin.pos);
        *avail_out = (uint32_t)(z->zout.size - z->zout.pos);
    }
}

static bm_zs_error zs_check(bm_zstream *z) {
    if (zs_zlib(z)) return zs_check_zlib(z);
    if (zs_brotli(z)) return zs_check_brotli(z);
    if (z->mode == ZS_NONE) return zs_zlib_message(z, "Zlib error");
    return zs_check_zstd(z);
}

static bm_zs_error zs_params(bm_zstream *z, int level, int strategy) {
    return zs_zlib(z) ? zs_params_zlib(z, level, strategy) : zs_none();
}

static bm_zs_error zs_reset(bm_zstream *z) {
    if (zs_zlib(z)) return zs_reset_zlib(z);
    if (zs_brotli(z)) return zs_init_brotli_state(z);
    if (z->mode == ZS_ZSTD_COMPRESS || z->mode == ZS_ZSTD_DECOMPRESS) {
        if (z->mode == ZS_ZSTD_DECOMPRESS) z->pledged = ZSTD_CONTENTSIZE_UNKNOWN;
        return zs_init_zstd_state(z);
    }
    return zs_none();
}

static void zs_close(bm_zstream *z) {
    if (!z) return;
    zs_close_zlib(z);
    zs_close_brotli(z);
    zs_close_zstd(z);
    free(z->dict);
    free(z);
}

static uint32_t zs_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    while (n > UINT32_MAX) {
        crc = (uint32_t)crc32(crc, p, UINT32_MAX);
        p += UINT32_MAX;
        n -= UINT32_MAX;
    }
    return (uint32_t)crc32(crc, p, (uInt)n);
}

static const bm_zs_ops bm_zs_table = {
    zs_open, zs_init_zlib, zs_init_brotli, zs_init_zstd, zs_write, zs_check, zs_params, zs_reset, zs_close, zs_crc32,
};

void bm_zlib_install(void) { bm_zs = &bm_zs_table; }

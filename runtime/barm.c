/* barm.c — the Barm runtime (C11, libc + libm only).
 *
 * Generated programs #include "barm.h" and this file into a single translation unit,
 * so everything that is not part of the barm.h contract is `static` and prefixed `bm_`.
 * See barm.h for ownership conventions.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   /* POSIX and BSD interfaces (sigaction, MAP_ANON, nanosleep) under -std=c11 */
#endif
#include "barm.h"

#include <float.h>
#include <math.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define BM_HAVE_ISATTY 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define BM_LIKELY(x) __builtin_expect(!!(x), 1)
#define BM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define BM_LIKELY(x) (x)
#define BM_UNLIKELY(x) (x)
#endif

/* ================================================================== globals */

int bm_argc;
char **bm_argv;

static jmp_buf *bm_test_jmp;       /* non-NULL while a test is running */
/* Unwinds to the running test (set by bm_test_run, so programs without tests don't link longjmp). */
void (*bm_test_unwind)(void); /* not static: the compiler would call its only value directly */
static bm_sb bm_test_msg;          /* failure message of the running test */
static const char *bm_test_loc;
static int64_t bm_tests_passed, bm_tests_failed;

/* ================================================================== traps and memory */

/* Output goes straight to the file descriptors (the runtime buffers stdout itself, and stderr
 * is unbuffered anyway): no stdio in the paths every program links. */
__attribute__((noinline)) void bm_write_fd(int fd, const char *s, size_t n) {
#if defined(__unix__) || defined(__APPLE__)
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
#else
    FILE *f = fd == 2 ? stderr : stdout;
    fwrite(s, 1, n, f);
    fflush(f);
#endif
}

__attribute__((noinline)) void bm_err_cstr(const char *s) { bm_write_fd(2, s, strlen(s)); }

static size_t bm_fmt_i64(char *buf, int64_t v);

static _Noreturn void bm_oom(size_t size) {
    bm_out_flush();
    char num[24];
    size_t n = bm_fmt_i64(num, (int64_t)size);
    bm_err_cstr("trap: out of memory (allocating ");
    bm_write_fd(2, num, n);
    bm_err_cstr(" bytes)\n");
    exit(101);
}

_Noreturn void bm_trap(const char *msg, const char *loc) {
    if (bm_test_jmp) {
        bm_test_msg.len = 0;
        bm_sb_push_cstr(&bm_test_msg, "trap: ");
        bm_sb_push_cstr(&bm_test_msg, msg);
        bm_test_loc = loc;
        bm_test_unwind();
    }
    bm_out_flush();
    bm_err_cstr("trap: ");
    bm_err_cstr(msg);
    bm_err_cstr("\n");
    if (loc && *loc) {
        bm_err_cstr("  at ");
        bm_err_cstr(loc);
        bm_err_cstr("\n");
    }
    exit(101);
}

void *bm_alloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (BM_UNLIKELY(!p)) bm_oom(size);
    return p;
}

void *bm_realloc(void *p, size_t size) {
    void *q = realloc(p, size ? size : 1);
    if (BM_UNLIKELY(!q)) bm_oom(size);
    return q;
}

void bm_free(void *p) { free(p); }

/* a * b + c, trapping on overflow (allocation sizes). */
static size_t bm_size_mul_add(size_t a, size_t b, size_t c) {
    if (b && a > (SIZE_MAX - c) / b) bm_trap("allocation size overflow", NULL);
    return a * b + c;
}

/* ================================================================== string builder */

/* A builder's buffer is laid out as a bm_strbuf (header, bytes, room for a NUL) so that
 * bm_str_from_sb can adopt it without copying. sb->data points at the bytes. */
#define BM_STR_HDR offsetof(bm_strbuf, data)

/* Short builders (most: a number, a header line, a small JSON body) live in the small-object
 * heap; a buffer's size (so where it lives) follows from its capacity. */
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
#define BM_SB_SMALL(cap) false
#else
#define BM_SB_SMALL(cap) (BM_STR_HDR + (cap) + 1 <= (BM_SMALL_CLASSES - 1) * 8)
#endif
static inline void bm_sb_release_buf(char *base, size_t cap) {
    if (BM_SB_SMALL(cap)) {
        size_t c = (BM_STR_HDR + cap + 1 + 7) >> 3;
        *(void **)(void *)base = bm_small_bins[c];
        bm_small_bins[c] = base;
    } else {
        free(base);
    }
}

void bm_sb_grow(bm_sb *sb, size_t need) {
    size_t cap = sb->cap ? sb->cap * 2 : 32;
    if (cap < need) cap = need;
    if (cap > SIZE_MAX - BM_STR_HDR - 1) bm_trap("string too long", NULL);
    char *old = sb->data ? sb->data - BM_STR_HDR : NULL;
    char *base;
    if (BM_SB_SMALL(cap)) {
        size_t c = (BM_STR_HDR + cap + 1 + 7) >> 3;
        void **f = bm_small_bins[c];
        if (f) bm_small_bins[c] = *f;
        else f = bm_small_refill(c);
        base = (char *)f;
        if (old) memcpy(base + BM_STR_HDR, sb->data, sb->len);
        if (old) bm_sb_release_buf(old, sb->cap);
    } else if (old && BM_SB_SMALL(sb->cap)) {
        base = (char *)bm_alloc(BM_STR_HDR + cap + 1);
        memcpy(base + BM_STR_HDR, sb->data, sb->len);
        bm_sb_release_buf(old, sb->cap);
    } else {
        base = (char *)bm_realloc(old, BM_STR_HDR + cap + 1);
    }
    sb->data = base + BM_STR_HDR;
    sb->cap = cap;
}

static inline char *bm_sb_reserve(bm_sb *sb, size_t extra) {
    if (BM_UNLIKELY(sb->cap - sb->len < extra)) {
        if (extra > SIZE_MAX - sb->len) bm_trap("string too long", NULL);
        bm_sb_grow(sb, sb->len + extra);
    }
    return sb->data + sb->len;
}

void bm_sb_push(bm_sb *sb, const char *s, size_t n) {
    if (!n) return;
    memcpy(bm_sb_reserve(sb, n), s, n);
    sb->len += n;
}

void bm_sb_push_cstr(bm_sb *sb, const char *s) { bm_sb_push(sb, s, strlen(s)); }

void bm_sb_push_char(bm_sb *sb, char c) {
    *bm_sb_reserve(sb, 1) = c;
    sb->len++;
}

void bm_sb_free(bm_sb *sb) {
    if (sb->data) bm_sb_release_buf(sb->data - BM_STR_HDR, sb->cap);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

void bm_sb_push_str(bm_sb *sb, bm_str s) { bm_sb_push(sb, s.p->data, (size_t)s.p->len); }

/* ================================================================== hashing */

static inline uint64_t bm_mix64(uint64_t x) { /* splitmix64 finalizer */
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static inline uint64_t bm_mum(uint64_t a, uint64_t b) {
#ifdef __SIZEOF_INT128__
    __extension__ typedef unsigned __int128 bm_u128;
    bm_u128 r = (bm_u128)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
#else
    uint64_t ha = a >> 32, la = (uint32_t)a, hb = b >> 32, lb = (uint32_t)b;
    uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
    uint64_t t = rl + (rm0 << 32), c = t < rl;
    uint64_t lo = t + (rm1 << 32);
    c += lo < t;
    uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
    return lo ^ hi;
#endif
}

static inline uint64_t bm_r64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t bm_r32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* wyhash-style byte hash. */
static uint64_t bm_hash_bytes(const void *data, size_t len) {
    const uint64_t s0 = 0xa0761d6478bd642fULL, s1 = 0xe7037ed1a0b428dbULL, s2 = 0x8ebc6af09c88c6e3ULL;
    const unsigned char *p = (const unsigned char *)data;
    uint64_t seed = 0x243f6a8885a308d3ULL ^ bm_mum(len ^ s0, s1);
    uint64_t a, b;
    if (len <= 16) {
        if (len >= 8) {
            a = bm_r64(p);
            b = bm_r64(p + len - 8);
        } else if (len >= 4) {
            a = bm_r32(p);
            b = bm_r32(p + len - 4);
        } else if (len > 0) {
            a = ((uint64_t)p[0] << 16) | ((uint64_t)p[len >> 1] << 8) | p[len - 1];
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t n = len;
        while (n > 16) {
            seed = bm_mum(bm_r64(p) ^ s1, bm_r64(p + 8) ^ seed);
            p += 16;
            n -= 16;
        }
        a = bm_r64(p + n - 16);
        b = bm_r64(p + n - 8);
    }
    return bm_mum(s1 ^ len, bm_mum(a ^ s2, b ^ seed));
}

/* ================================================================== sorting */

/* LSD radix sort of 64-bit keys, 11-bit digits, skipping digits every key shares.
 * `idx` (optional) is permuted alongside the keys. Stable. */
void bm_radix64(uint64_t *keys, int64_t *idx, bm_int n) {
    if (n < 2) return;
    enum { BITS = 11, B = 1 << BITS, PASSES = 6 };
    size_t *hist = calloc((size_t)PASSES * B, sizeof(size_t));
    if (!hist) bm_trap("out of memory", "sort");
    for (bm_int i = 0; i < n; i++) {
        uint64_t k = keys[i];
        for (int p = 0; p < PASSES; p++) hist[p * B + ((k >> (p * BITS)) & (B - 1))]++;
    }
    uint64_t *k2 = bm_alloc((size_t)n * sizeof(uint64_t));
    int64_t *i2 = idx ? bm_alloc((size_t)n * sizeof(int64_t)) : NULL;
    uint64_t *ks = keys, *kd = k2;
    int64_t *is = idx, *id = i2;
    for (int p = 0; p < PASSES; p++) {
        size_t *h = hist + p * B;
        int shift = p * BITS;
        if (h[(ks[0] >> shift) & (B - 1)] == (size_t)n) continue;
        size_t sum = 0;
        for (int b = 0; b < B; b++) { size_t c = h[b]; h[b] = sum; sum += c; }
        if (idx) {
            for (bm_int i = 0; i < n; i++) { uint64_t k = ks[i]; size_t pos = h[(k >> shift) & (B - 1)]++; kd[pos] = k; id[pos] = is[i]; }
            int64_t *ti = is; is = id; id = ti;
        } else {
            for (bm_int i = 0; i < n; i++) { uint64_t k = ks[i]; kd[h[(k >> shift) & (B - 1)]++] = k; }
        }
        uint64_t *tk = ks; ks = kd; kd = tk;
    }
    if (ks != keys) memcpy(keys, ks, (size_t)n * sizeof(uint64_t));
    if (idx && is != idx) memcpy(idx, is, (size_t)n * sizeof(int64_t));
    bm_free(k2);
    if (i2) bm_free(i2);
    free(hist);
}
/* Order-preserving 64-bit key for a double under the comparator `a - b`:
 * both zeros share a key (they compare equal), NaN sorts last. */
static inline uint64_t bm_f64_key(double x) {
    uint64_t u;
    if (x == 0) u = 0;
    else if (x != x) u = 0x7FF8000000000000ull;
    else memcpy(&u, &x, 8);
    return (u >> 63) ? ~u : (u ^ 0x8000000000000000ull);
}
/* In-place radix sort of 64-bit keys, not stable (callers use it only where equal keys are
 * indistinguishable). One in-place pass (American flag) splits the keys by the 11 highest bits
 * on which they differ; buckets then finish with LSD radix sort through a scratch buffer the
 * size of the bucket (small, in cache) — or, if a bucket is still large, recursively in place.
 * Extra memory: the largest small bucket, not a copy of the array. */
enum { BM_RX_SMALL = 1 << 15 };
static void bm_lsd_bucket(uint64_t *k, size_t n, uint64_t *tmp, uint64_t diff) {
    uint64_t *src = k, *dst = tmp;
    for (int shift = 0; shift < 64; shift += 8) {
        if (!((diff >> shift) & 255)) continue; /* a byte every key shares */
        size_t cnt[256] = {0};
        for (size_t i = 0; i < n; i++) cnt[(src[i] >> shift) & 255]++;
        size_t pos = 0;
        for (int b = 0; b < 256; b++) { size_t c = cnt[b]; cnt[b] = pos; pos += c; }
        for (size_t i = 0; i < n; i++) { uint64_t v = src[i]; dst[cnt[(v >> shift) & 255]++] = v; }
        uint64_t *t = src; src = dst; dst = t;
    }
    if (src != k) memcpy(k, src, n * sizeof *k);
}
/* Scratch for bucket sorts, grown to the largest bucket met (often a few KiB). */
typedef struct bm_rx_tmp { uint64_t *p; size_t cap; } bm_rx_tmp;
/* One American-flag pass on the digit at `shift` (counts `cnt`, bucket cursors `head`/`tail`
 * of type T), then each bucket recursively. */
#define BM_RX_PARTITION(T) \
    for (size_t i = 0; i < n; i++) cnt[(k[i] >> shift) & (B - 1)]++; \
    { T pos = 0; for (int b = 0; b < B; b++) { head[b] = pos; pos += cnt[b]; tail[b] = pos; } } \
    for (int b = 0; b < B; b++) { \
        while (head[b] < tail[b]) { \
            uint64_t v = k[head[b]]; \
            size_t d = (v >> shift) & (B - 1); \
            while (d != (size_t)b) { /* cycle: place v, pick up what was there */ \
                uint64_t t = k[head[d]]; \
                k[head[d]++] = v; \
                v = t; \
                d = (v >> shift) & (B - 1); \
            } \
            k[head[b]++] = v; \
        } \
    } \
    { size_t start = 0; \
      for (int b = 0; b < B; b++) { \
          if (shift > 0 && cnt[b] > 1) bm_radix_inplace(k + start, cnt[b], tmp); \
          start += cnt[b]; \
      } }
static void bm_radix_inplace(uint64_t *k, size_t n, bm_rx_tmp *tmp) {
    if (n < 64) {
        for (size_t i = 1; i < n; i++) {
            uint64_t v = k[i];
            size_t j = i;
            while (j > 0 && k[j - 1] > v) { k[j] = k[j - 1]; j--; }
            k[j] = v;
        }
        return;
    }
    uint64_t diff = 0, k0 = k[0];
    for (size_t i = 1; i < n; i++) diff |= k[i] ^ k0;
    if (!diff) return;
    if (n <= BM_RX_SMALL) {
        if (tmp->cap < n) {
            size_t cap = tmp->cap * 2 > n ? tmp->cap * 2 : n;
            if (cap > BM_RX_SMALL) cap = BM_RX_SMALL;
            tmp->p = bm_realloc(tmp->p, cap * sizeof(uint64_t));
            tmp->cap = cap;
        }
        bm_lsd_bucket(k, n, tmp->p, diff);
        return;
    }
    int top = 63 - __builtin_clzll(diff);
    int shift = top >= 10 ? top - 10 : 0;
    enum { B = 2048 };
    /* 32-bit counts on the stack while they fit (24 KiB, no allocation), else 64-bit on the heap */
    if (n <= UINT32_MAX) {
        uint32_t cnt[B] = {0}, head[B], tail[B];
        BM_RX_PARTITION(uint32_t)
        return;
    }
    size_t *cnt = calloc(B * 3, sizeof(size_t));
    if (!cnt) bm_trap("out of memory", "sort");
    size_t *head = cnt + B, *tail = cnt + 2 * B;
    BM_RX_PARTITION(size_t)
    free(cnt);
}
/* xs.sort((a, b) => a - b) / (b - a) on f64[]: the doubles become order-preserving keys in
 * place, are radix-sorted in place, and are decoded back. Equal keys are equal values (NaNs
 * share one key; both zeros share a key and keep their original order: their signs are
 * replayed in encounter order), so the unstable in-place sort is unobservable. No copy of the
 * array is made. */
void bm_sort_f64(double *a, bm_int n, bool desc) {
    if (n < 2) return;
    uint64_t *k = (uint64_t *)(void *)a;
    uint8_t *zs = NULL;
    bm_int nz = 0;
    for (bm_int i = 0; i < n; i++) {
        double x = a[i];
        if (x == 0) {
            if (!zs) zs = bm_alloc((size_t)n);
            zs[nz++] = signbit(x) ? 1 : 0;
        }
        uint64_t key = bm_f64_key(x);
        k[i] = desc ? ~key : key;
    }
    bm_rx_tmp tmp = { NULL, 0 };
    bm_radix_inplace(k, (size_t)n, &tmp);
    free(tmp.p);
    uint64_t kz = desc ? ~0x8000000000000000ull : 0x8000000000000000ull;
    bm_int zi = 0;
    for (bm_int i = 0; i < n; i++) {
        uint64_t key = desc ? ~k[i] : k[i];
        if (k[i] == kz) { a[i] = zs[zi++] ? -0.0 : 0.0; continue; }
        uint64_t u = (key >> 63) ? (key ^ 0x8000000000000000ull) : ~key;
        memcpy(&a[i], &u, 8);
    }
    if (zs) bm_free(zs);
}

/* ================================================================== strings */

const bm_strbuf bm_empty_strbuf = {-1, 0, {0}}; /* flexible-array initializer: GNU C (gcc, clang) */

typedef struct { int32_t rc; int32_t len; char data[4]; } bm_small_strbuf;
#define BM_A1(c) {-1, 1, {(char)(c), 0}}
#define BM_A4(c) BM_A1(c), BM_A1((c) + 1), BM_A1((c) + 2), BM_A1((c) + 3)
#define BM_A16(c) BM_A4(c), BM_A4((c) + 4), BM_A4((c) + 8), BM_A4((c) + 12)
/* Immortal one-character strings for printable ASCII: chars()/split("") and friends don't allocate for them. */
static const bm_small_strbuf bm_ascii_strs[96] = { /* printable ASCII, 32..127 */
    BM_A16(32), BM_A16(48), BM_A16(64), BM_A16(80), BM_A16(96), BM_A16(112),
};
#undef BM_A1
#undef BM_A4
#undef BM_A16

BM_STR_LIT(bm_lit_true, "true");
BM_STR_LIT(bm_lit_false, "false");

static inline bm_str bm_ascii_str(unsigned char c) { return (bm_str){(bm_strbuf *)&bm_ascii_strs[c - 32]}; }

/* Small strings (header + bytes + NUL <= BM_SMALL_MAX) come from the small-object free lists;
 * larger ones from malloc. The class follows from the length alone, so freeing needs no flag.
 * (Plain malloc under AddressSanitizer, so it sees every string.) */
enum { BM_SMALL_MAX = 256 };
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
#define BM_PLAIN_ALLOC 1
#endif
void *bm_small_bins[BM_SMALL_CLASSES];

/* A size class's free list is empty: hand out its next never-used block. Blocks come from a
 * chunk per class (1 KiB, doubling with each chunk to 16 KiB) and are handed out one by one, so
 * memory is only touched once an object uses it. The first 64 KiB of chunks come from malloc
 * one by one (mostly pages already in use: a small program adds none of its own); later ones
 * are cut from 16 MiB malloc blocks, mapped lazily (untouched pages cost nothing) and able to
 * reuse memory the program freed, such as a map's outgrown table. */
typedef struct bm_class_space { char *cur, *lim; uint8_t grow; } bm_class_space;
enum { BM_ARENA_BLOCK = 16 << 20 };
static char *bm_bump, *bm_bump_end;
static size_t bm_carved;
static void *bm_carve(bm_class_space *k, size_t sz) {
    if (BM_LIKELY((size_t)(k->lim - k->cur) >= sz)) {
        char *p = k->cur;
        k->cur += sz;
        return p;
    }
    size_t chunk = (size_t)1024 << k->grow;
    if (chunk >= 16384) chunk = 16384;
    else k->grow++;
    size_t n = chunk / sz;
    if (n == 0) n = 1;
    size_t bytes = n * sz;
    char *p;
    if (bm_carved + bytes <= 65536) {
        p = bm_alloc(bytes);
        bm_carved += bytes;
    } else {
        if ((size_t)(bm_bump_end - bm_bump) < bytes) {
            bm_bump = bm_alloc(BM_ARENA_BLOCK);
            bm_bump_end = bm_bump + BM_ARENA_BLOCK;
        }
        p = bm_bump;
        bm_bump += bytes;
    }
    k->cur = p + sz;
    k->lim = p + bytes;
    return p;
}

static bm_class_space bm_small_space[BM_SMALL_CLASSES];
__attribute__((noinline)) void *bm_small_refill(size_t c) {
    return bm_carve(&bm_small_space[c], c * 8);
}
static inline void *bm_small_alloc(size_t size) {
#ifdef BM_PLAIN_ALLOC
    return bm_alloc(size);
#endif
    size_t c = (size + 7) >> 3;
    void **f = bm_small_bins[c];
    if (BM_LIKELY(f != NULL)) { bm_small_bins[c] = *f; return f; }
    return bm_small_refill(c);
}
static inline void bm_small_free(void *p, size_t size) {
#ifdef BM_PLAIN_ALLOC
    free(p); return;
#endif
    size_t c = (size + 7) >> 3;
    *(void **)p = bm_small_bins[c];
    bm_small_bins[c] = p;
}
static inline size_t bm_strbuf_size(size_t n) { return BM_STR_HDR + n + 1; }

static bm_strbuf *bm_strbuf_new(size_t n) {
    size_t size = bm_size_mul_add(n, 1, BM_STR_HDR + 1);
    if (n > INT32_MAX) bm_trap("string too long", NULL);
    bm_strbuf *b = (bm_strbuf *)(size <= BM_SMALL_MAX ? bm_small_alloc(size) : bm_alloc(size));
    b->rc = 1;
    b->len = (int32_t)n;
    b->data[n] = 0;
    return b;
}

void bm_str_release_slow(bm_str s) {
    size_t size = bm_strbuf_size((size_t)s.p->len);
    if (size <= BM_SMALL_MAX) bm_small_free(s.p, size);
    else free(s.p);
}

bm_str bm_str_from(const char *bytes, size_t n) {
    if (n == 0) return BM_EMPTY_STR;
    if (n == 1 && (unsigned char)bytes[0] - 32u < 96u) return bm_ascii_str((unsigned char)bytes[0]);
    bm_strbuf *b = bm_strbuf_new(n);
    memcpy(b->data, bytes, n);
    return (bm_str){b};
}

bm_str bm_str_from_sb(bm_sb *sb) {
    size_t n = sb->len;
    if (n <= 1) { /* empty or one byte: bm_str_from may return an immortal string */
        bm_str r = bm_str_from(sb->data, n);
        bm_sb_free(sb);
        return r;
    }
    if (bm_strbuf_size(n) <= BM_SMALL_MAX) { /* small strings live in the small-block allocator */
        bm_str r = bm_str_from(sb->data, n);
        bm_sb_free(sb);
        return r;
    }
    if (BM_SB_SMALL(sb->cap)) { /* the builder's block is in the small-object heap: copy out */
        bm_strbuf *b = (bm_strbuf *)bm_alloc(BM_STR_HDR + n + 1);
        memcpy(b->data, sb->data, n);
        bm_sb_free(sb);
        b->rc = 1;
        b->len = (int32_t)n;
        b->data[n] = 0;
        return (bm_str){b};
    }
    bm_strbuf *b = (bm_strbuf *)(sb->data - BM_STR_HDR);
    if (sb->cap - n > 64) b = (bm_strbuf *)bm_realloc(b, BM_STR_HDR + n + 1);
    b->rc = 1;
    if (n > INT32_MAX) bm_trap("string too long", NULL);
    b->len = (int32_t)n;
    b->data[n] = 0;
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return (bm_str){b};
}

bm_str bm_str_concat(bm_str a, bm_str b) {
    size_t na = (size_t)a.p->len, nb = (size_t)b.p->len;
    if (nb == 0) { bm_str_retain(a); return a; }
    if (na == 0) { bm_str_retain(b); return b; }
    bm_strbuf *r = bm_strbuf_new(bm_size_mul_add(na, 1, nb));
    memcpy(r->data, a.p->data, na);
    memcpy(r->data + na, b.p->data, nb);
    return (bm_str){r};
}

bool bm_str_eq(bm_str a, bm_str b) {
    return a.p == b.p || (a.p->len == b.p->len && memcmp(a.p->data, b.p->data, (size_t)a.p->len) == 0);
}

int bm_str_cmp(bm_str a, bm_str b) {
    if (a.p == b.p) return 0;
    int64_t na = a.p->len, nb = b.p->len;
    int c = memcmp(a.p->data, b.p->data, (size_t)(na < nb ? na : nb));
    if (c) return c < 0 ? -1 : 1;
    return na < nb ? -1 : na > nb;
}

uint64_t bm_str_hash(bm_str s) { return bm_hash_bytes(s.p->data, (size_t)s.p->len); }

/* ------------------------------------------------------------------ integer / bool formatting */

/* Writes the decimal digits of v ending at `end`; returns the start. */
static char *bm_fmt_u64_rev(char *end, uint64_t v) {
    do {
        *--end = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return end;
}

static size_t bm_fmt_i64(char *buf, int64_t v) { /* buf >= 21 bytes */
    char tmp[24], *end = tmp + sizeof tmp;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char *p = bm_fmt_u64_rev(end, u);
    if (v < 0) *--p = '-';
    size_t n = (size_t)(end - p);
    memcpy(buf, p, n);
    return n;
}

bm_str bm_str_from_int(bm_int v) {
    char buf[24];
    return bm_str_from(buf, bm_fmt_i64(buf, v));
}

void bm_sb_push_int(bm_sb *sb, bm_int v) {
    char buf[24];
    bm_sb_push(sb, buf, bm_fmt_i64(buf, v));
}

static void bm_sb_push_u64(bm_sb *sb, uint64_t v) {
    char tmp[24], *end = tmp + sizeof tmp;
    char *p = bm_fmt_u64_rev(end, v);
    bm_sb_push(sb, p, (size_t)(end - p));
}

bm_str bm_str_from_bool(bool v) { return v ? BM_LIT(bm_lit_true) : BM_LIT(bm_lit_false); }

/* ------------------------------------------------------------------ JS number formatting */

/* Shortest decimal digits d1..dk (no trailing zeros) and exponent n such that
 * v == 0.d1..dk × 10^n round-trips (v > 0, finite). For f32, round-trips as a float. */
static int bm_shortest_digits(double v, bool is_f32, char *digits, int *n_out) {
    char tmp[48];
    if (is_f32) {
        for (int prec = 0; prec < 9; prec++) {
            snprintf(tmp, sizeof tmp, "%.*e", prec, v);
            if (strtof(tmp, NULL) == (float)v) break;
        }
    } else if (v >= DBL_MIN) {
        /* For normal doubles, if any <=15-digit decimal round-trips then the correctly rounded
         * 15-digit one does too, and stripping its trailing zeros gives the shortest. */
        snprintf(tmp, sizeof tmp, "%.14e", v);
        if (strtod(tmp, NULL) != v) {
            snprintf(tmp, sizeof tmp, "%.15e", v);
            if (strtod(tmp, NULL) != v) snprintf(tmp, sizeof tmp, "%.16e", v);
        }
    } else { /* subnormal: fewer significant bits, search all precisions */
        for (int prec = 0; prec < 17; prec++) {
            snprintf(tmp, sizeof tmp, "%.*e", prec, v);
            if (strtod(tmp, NULL) == v) break;
        }
    }
    int k = 0;
    const char *s = tmp;
    for (; *s && *s != 'e'; s++)
        if (*s >= '0' && *s <= '9') digits[k++] = *s;
    int e = *s == 'e' ? atoi(s + 1) : 0;
    while (k > 1 && digits[k - 1] == '0') k--;
    *n_out = e + 1;
    return k;
}

/* JS Number::toString(v) into buf (>= 32 bytes); returns the length. */
static size_t bm_fmt_number(char *buf, double v, bool is_f32) {
    if (v != v) { memcpy(buf, "NaN", 3); return 3; }
    if (v == 0) { buf[0] = '0'; return 1; }
    char *p = buf;
    if (v < 0) { *p++ = '-'; v = -v; }
    if (isinf(v)) { memcpy(p, "Infinity", 8); return (size_t)(p - buf) + 8; }
    if (v < (is_f32 ? 16777216.0 : 9007199254740992.0) && v == (double)(int64_t)v)
        return (size_t)(p - buf) + bm_fmt_i64(p, (int64_t)v);
    char d[24] = {0};
    int n, k = 0;
    /* Fast path: the fewest decimals f such that round(v * 10^f) / 10^f is exactly v (both
     * exact in a double: the division is correctly rounded, like parsing the decimal). The
     * smallest f gives the fewest significant digits. Typical JSON numbers (prices, coordinates)
     * take this path; others use the general shortest search. */
    static const double pow10_tab[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16 };
    if (!is_f32 && v >= 1e-7 && v < 1e15) {
        for (int f = 1; f <= 16 && !k; f++) {
            double s = v * pow10_tab[f];
            if (s >= 9007199254740992.0) break;
            double r = (double)(int64_t)(s + 0.5);
            if (r / pow10_tab[f] != v) continue;
            /* digits of r, with the decimal point f places from the right */
            char tmp[24];
            int m = (int)bm_fmt_i64(tmp, (int64_t)r);
            int lead = 0;
            while (lead < m - 1 && tmp[lead] == '0') lead++;
            k = m - lead;
            memcpy(d, tmp + lead, (size_t)k);
            n = m - f;
            while (k > 1 && d[k - 1] == '0') k--;
        }
    }
    if (!k) k = bm_shortest_digits(v, is_f32, d, &n);
    if (k <= n && n <= 21) {
        memcpy(p, d, (size_t)k); p += k;
        for (int i = k; i < n; i++) *p++ = '0';
    } else if (0 < n && n <= 21) {
        memcpy(p, d, (size_t)n); p += n;
        *p++ = '.';
        memcpy(p, d + n, (size_t)(k - n)); p += k - n;
    } else if (-6 < n && n <= 0) {
        *p++ = '0'; *p++ = '.';
        for (int i = n; i < 0; i++) *p++ = '0';
        memcpy(p, d, (size_t)k); p += k;
    } else {
        *p++ = d[0];
        if (k > 1) { *p++ = '.'; memcpy(p, d + 1, (size_t)(k - 1)); p += k - 1; }
        *p++ = 'e';
        int e = n - 1;
        *p++ = e < 0 ? '-' : '+';
        p += bm_fmt_i64(p, e < 0 ? -e : e);
    }
    return (size_t)(p - buf);
}

bm_str bm_str_from_f64(double v) {
    char buf[40];
    return bm_str_from(buf, bm_fmt_number(buf, v, false));
}

void bm_sb_push_f64(bm_sb *sb, double v) {
    char buf[40];
    bm_sb_push(sb, buf, bm_fmt_number(buf, v, false));
}

static void bm_toFixed_check(bm_int digits, const char *loc) {
    if (digits < 0 || digits > 100) bm_trap("toFixed() digits argument must be between 0 and 100", loc);
}

bm_str bm_f64_to_fixed(double v, bm_int digits, const char *loc) {
    bm_toFixed_check(digits, loc);
    if (v != v || fabs(v) >= 1e21) return bm_str_from_f64(v);
    if (v == 0) v = 0.0; /* (-0).toFixed() is "0" */
    int f = (int)digits;
    char buf[160];
    int len;
    /* printf rounds exact ties to even; JS rounds them away from zero. A tie happens exactly
     * when v = m × 2^-(f+1) with m odd; then "%.{f+1}f" is exact and ends in '5'. */
    int e2;
    double fr = frexp(fabs(v), &e2);
    uint64_t mant = (uint64_t)ldexp(fr, 53);
    int e = e2 - 53;
    while (mant && !(mant & 1)) { mant >>= 1; e++; }
    if (mant && e == -(f + 1)) {
        len = snprintf(buf, sizeof buf, "%.*f", f + 1, v);
        buf[--len] = 0;                 /* drop the '5' */
        if (f == 0) buf[--len] = 0;     /* drop the '.' */
        int i = len - 1;
        for (;;) {                      /* increment the magnitude */
            if (i < 0 || buf[i] == '-') {
                memmove(buf + (i + 2), buf + (i + 1), (size_t)(len - i)); /* shift incl. NUL */
                buf[i + 1] = '1';
                len++;
                break;
            }
            if (buf[i] == '.') { i--; continue; }
            if (buf[i] == '9') { buf[i--] = '0'; continue; }
            buf[i]++;
            break;
        }
    } else {
        len = snprintf(buf, sizeof buf, "%.*f", f, v);
    }
    return bm_str_from(buf, (size_t)len);
}

bm_str bm_int_to_fixed(bm_int v, bm_int digits, const char *loc) {
    bm_toFixed_check(digits, loc);
    bm_sb sb = {0};
    bm_sb_push_int(&sb, v);
    if (digits > 0) {
        bm_sb_push_char(&sb, '.');
        memset(bm_sb_reserve(&sb, (size_t)digits), '0', (size_t)digits);
        sb.len += (size_t)digits;
    }
    return bm_str_from_sb(&sb);
}

/* ------------------------------------------------------------------ UTF-8 helpers */

static inline size_t bm_utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1; /* stray continuation byte (invalid UTF-8): treat as one unit */
}

/* Decodes the character at p (n bytes available); returns its byte length. */
static size_t bm_utf8_decode(const unsigned char *p, size_t n, uint32_t *cp) {
    size_t l = bm_utf8_len(p[0]);
    if (l > n) l = n;
    uint32_t c = l == 1 ? p[0] : l == 2 ? p[0] & 0x1F : l == 3 ? p[0] & 0x0F : p[0] & 0x07;
    for (size_t i = 1; i < l; i++) c = (c << 6) | (p[i] & 0x3F);
    *cp = c;
    return l;
}

static inline bool bm_is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static bool bm_is_js_space(uint32_t c) {
    if (c < 0x80) return c == ' ' || (c >= 0x09 && c <= 0x0D);
    return c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

static void bm_trim_range(bm_str s, bool left, bool right, size_t *a_out, size_t *b_out) {
    const unsigned char *d = (const unsigned char *)s.p->data;
    size_t a = 0, b = (size_t)s.p->len;
    uint32_t cp;
    if (left)
        while (a < b) {
            size_t l = bm_utf8_decode(d + a, b - a, &cp);
            if (!bm_is_js_space(cp)) break;
            a += l;
        }
    if (right)
        while (b > a) {
            size_t st = b - 1;
            while (st > a && bm_is_cont(d[st]) && b - st < 4) st--;
            bm_utf8_decode(d + st, b - st, &cp);
            if (!bm_is_js_space(cp)) break;
            b = st;
        }
    *a_out = a;
    *b_out = b;
}

bm_int bm_str_char_count(bm_str s) {
    const unsigned char *d = (const unsigned char *)s.p->data;
    size_t n = (size_t)s.p->len, count = 0, i = 0;
#if defined(__GNUC__) || defined(__clang__)
    for (; i + 8 <= n; i += 8) { /* count non-continuation bytes, 8 at a time */
        uint64_t w = bm_r64(d + i);
        uint64_t cont = (w & ~(w << 1)) & 0x8080808080808080ULL; /* bit7=1, bit6=0 */
        count += 8 - (size_t)__builtin_popcountll(cont);
    }
#endif
    for (; i < n; i++) count += !bm_is_cont(d[i]);
    return (bm_int)count;
}

/* Byte offset after `chars` characters starting at d (bounded by n). */
static size_t bm_utf8_advance(const char *d, size_t n, size_t chars) {
    size_t i = 0;
    while (chars-- && i < n) i += bm_utf8_len((unsigned char)d[i]);
    return i < n ? i : n;
}

/* ------------------------------------------------------------------ string methods */

static inline int64_t bm_clamp_index(int64_t i, int64_t len) {
    if (i < 0) { i += len; return i < 0 ? 0 : i; }
    return i > len ? len : i;
}

static void bm_check_boundary(bm_str s, int64_t i, const char *loc) {
    if (i > 0 && i < s.p->len && bm_is_cont((unsigned char)s.p->data[i])) {
        char msg[96];
        snprintf(msg, sizeof msg, "string index %lld is not on a UTF-8 character boundary", (long long)i);
        bm_trap(msg, loc);
    }
}

bm_str bm_str_slice(bm_str s, bm_int start, bm_int end, bool has_end, const char *loc) {
    int64_t len = s.p->len;
    int64_t a = bm_clamp_index(start, len);
    int64_t b = has_end ? bm_clamp_index(end, len) : len;
    if (b < a) b = a;
    bm_check_boundary(s, a, loc);
    bm_check_boundary(s, b, loc);
    if (a == 0 && b == len) { bm_str_retain(s); return s; }
    return bm_str_from(s.p->data + a, (size_t)(b - a));
}

/* Byte index of needle in h at or after `from`, or -1. */
static int64_t bm_find(const char *h, size_t hn, const char *nd, size_t nn, size_t from) {
    if (from > hn || nn > hn - from) return -1;
    if (nn == 0) return (int64_t)from;
    const char *p = h + from, *last = h + hn - nn; /* last possible start */
    char first = nd[0];
    while (p <= last) {
        p = (const char *)memchr(p, first, (size_t)(last - p) + 1);
        if (!p) return -1;
        if (memcmp(p + 1, nd + 1, nn - 1) == 0) return p - h;
        p++;
    }
    return -1;
}

bool bm_str_includes(bm_str s, bm_str needle) { return bm_str_index_of(s, needle) >= 0; }

bool bm_str_starts_with(bm_str s, bm_str prefix) {
    return prefix.p->len <= s.p->len && memcmp(s.p->data, prefix.p->data, (size_t)prefix.p->len) == 0;
}

bool bm_str_ends_with(bm_str s, bm_str suffix) {
    return suffix.p->len <= s.p->len &&
           memcmp(s.p->data + s.p->len - suffix.p->len, suffix.p->data, (size_t)suffix.p->len) == 0;
}

bm_int bm_str_index_of(bm_str s, bm_str needle) {
    return bm_find(s.p->data, (size_t)s.p->len, needle.p->data, (size_t)needle.p->len, 0);
}

static bm_arrbuf *bm_arrbuf_new(size_t esize, int64_t cap);

bm_arr bm_str_chars(bm_str s) {
    int64_t count = bm_str_char_count(s);
    if (count == 0) return BM_EMPTY_ARR;
    bm_arrbuf *b = bm_arrbuf_new(sizeof(bm_str), count);
    bm_str *out = (bm_str *)(void *)b->data;
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, i = 0;
    int64_t k = 0;
    while (i < n && k < count) {
        size_t l = bm_utf8_len((unsigned char)d[i]);
        if (l > n - i) l = n - i;
        out[k++] = bm_str_from(d + i, l);
        i += l;
    }
    return (bm_arr){b, k};
}

bm_arr bm_str_split(bm_str s, bm_str sep) {
    if (sep.p->len == 0) return bm_str_chars(s);
    bm_arr r = BM_EMPTY_ARR;
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, sn = (size_t)sep.p->len, from = 0;
    int64_t pos = bm_find(d, n, sep.p->data, sn, 0);
    if (pos < 0) {
        bm_str_retain(s);
        bm_arr_push(&r, &bm_type_str, &s);
        return r;
    }
    for (;;) {
        size_t end = pos < 0 ? n : (size_t)pos;
        bm_str piece = bm_str_from(d + from, end - from);
        bm_arr_push_fast(&r, &bm_type_str, &piece);
        if (pos < 0) break;
        from = end + sn;
        pos = bm_find(d, n, sep.p->data, sn, from);
    }
    return r;
}

static bm_str bm_str_trim_impl(bm_str s, bool left, bool right) {
    size_t a, b;
    bm_trim_range(s, left, right, &a, &b);
    if (a == 0 && b == (size_t)s.p->len) { bm_str_retain(s); return s; }
    return bm_str_from(s.p->data + a, b - a);
}

bm_str bm_str_trim(bm_str s) { return bm_str_trim_impl(s, true, true); }
bm_str bm_str_trim_start(bm_str s) { return bm_str_trim_impl(s, true, false); }
bm_str bm_str_trim_end(bm_str s) { return bm_str_trim_impl(s, false, true); }

static bm_str bm_str_map_ascii(bm_str s, char lo, char hi, int delta) {
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, i = 0;
    while (i < n && !(d[i] >= lo && d[i] <= hi)) i++;
    if (i == n) { bm_str_retain(s); return s; }
    bm_strbuf *b = bm_strbuf_new(n);
    memcpy(b->data, d, n);
    for (; i < n; i++)
        if (b->data[i] >= lo && b->data[i] <= hi) b->data[i] = (char)(b->data[i] + delta);
    return (bm_str){b};
}

bm_str bm_str_to_upper(bm_str s) { return bm_str_map_ascii(s, 'a', 'z', 'A' - 'a'); }
bm_str bm_str_to_lower(bm_str s) { return bm_str_map_ascii(s, 'A', 'Z', 'a' - 'A'); }

/* Appends a JS replacement string, expanding $$, $&, $` and $'. */
static void bm_push_replacement(bm_sb *sb, bm_str repl, const char *s, size_t slen, size_t pos, size_t mlen) {
    const char *r = repl.p->data;
    size_t rn = (size_t)repl.p->len;
    if (!memchr(r, '$', rn)) { bm_sb_push(sb, r, rn); return; }
    for (size_t i = 0; i < rn; i++) {
        if (r[i] == '$' && i + 1 < rn) {
            char c = r[i + 1];
            if (c == '$') { bm_sb_push_char(sb, '$'); i++; continue; }
            if (c == '&') { bm_sb_push(sb, s + pos, mlen); i++; continue; }
            if (c == '`') { bm_sb_push(sb, s, pos); i++; continue; }
            if (c == '\'') { bm_sb_push(sb, s + pos + mlen, slen - pos - mlen); i++; continue; }
        }
        bm_sb_push_char(sb, r[i]);
    }
}

bm_str bm_str_replace(bm_str s, bm_str search, bm_str replacement) {
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, mn = (size_t)search.p->len;
    int64_t pos = bm_find(d, n, search.p->data, mn, 0);
    if (pos < 0) { bm_str_retain(s); return s; }
    bm_sb sb = {0};
    bm_sb_push(&sb, d, (size_t)pos);
    bm_push_replacement(&sb, replacement, d, n, (size_t)pos, mn);
    bm_sb_push(&sb, d + pos + mn, n - (size_t)pos - mn);
    return bm_str_from_sb(&sb);
}

bm_str bm_str_replace_all(bm_str s, bm_str search, bm_str replacement) {
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, mn = (size_t)search.p->len;
    bm_sb sb = {0};
    if (mn == 0) { /* insert at every character boundary, including both ends */
        size_t i = 0;
        for (;;) {
            bm_push_replacement(&sb, replacement, d, n, i, 0);
            if (i >= n) break;
            size_t l = bm_utf8_len((unsigned char)d[i]);
            if (l > n - i) l = n - i;
            bm_sb_push(&sb, d + i, l);
            i += l;
        }
        return bm_str_from_sb(&sb);
    }
    int64_t pos = bm_find(d, n, search.p->data, mn, 0);
    if (pos < 0) { bm_str_retain(s); return s; }
    size_t last = 0;
    while (pos >= 0) {
        bm_sb_push(&sb, d + last, (size_t)pos - last);
        bm_push_replacement(&sb, replacement, d, n, (size_t)pos, mn);
        last = (size_t)pos + mn;
        pos = bm_find(d, n, search.p->data, mn, last);
    }
    bm_sb_push(&sb, d + last, n - last);
    return bm_str_from_sb(&sb);
}

bm_str bm_str_repeat(bm_str s, bm_int count, const char *loc) {
    if (count < 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "Invalid count value: %lld", (long long)count);
        bm_trap(msg, loc);
    }
    size_t n = (size_t)s.p->len;
    if (count == 0 || n == 0) return BM_EMPTY_STR;
    if (count == 1) { bm_str_retain(s); return s; }
    if ((uint64_t)count > (uint64_t)(INT64_MAX / 2) / n) bm_trap("Invalid string length", loc);
    size_t total = n * (size_t)count;
    bm_strbuf *b = bm_strbuf_new(total);
    memcpy(b->data, s.p->data, n);
    size_t have = n;
    while (have < total) { /* doubling copies */
        size_t c = have <= total - have ? have : total - have;
        memcpy(b->data + have, b->data, c);
        have += c;
    }
    return (bm_str){b};
}

/* Appends `chars` characters of fill (cycled). fill is non-empty. */
static void bm_push_fill(bm_sb *sb, bm_str fill, int64_t chars) {
    int64_t fc = bm_str_char_count(fill);
    int64_t full = chars / fc, rem = chars % fc;
    for (int64_t i = 0; i < full; i++) bm_sb_push_str(sb, fill);
    bm_sb_push(sb, fill.p->data, bm_utf8_advance(fill.p->data, (size_t)fill.p->len, (size_t)rem));
}

static bm_str bm_str_pad(bm_str s, bm_int len, bm_str fill, bool at_start) {
    int64_t cur = bm_str_char_count(s);
    if (len <= cur || fill.p->len == 0) { bm_str_retain(s); return s; }
    bm_sb sb = {0};
    if (!at_start) bm_sb_push_str(&sb, s);
    bm_push_fill(&sb, fill, len - cur);
    if (at_start) bm_sb_push_str(&sb, s);
    return bm_str_from_sb(&sb);
}

bm_str bm_str_pad_start(bm_str s, bm_int len, bm_str fill) { return bm_str_pad(s, len, fill, true); }
bm_str bm_str_pad_end(bm_str s, bm_int len, bm_str fill) { return bm_str_pad(s, len, fill, false); }

/* ------------------------------------------------------------------ parsing */

static inline bool bm_is_digit(char c) { return c >= '0' && c <= '9'; }

bool bm_parse_float(bm_str s, double *out) {
    size_t a, b;
    bm_trim_range(s, true, true, &a, &b);
    const char *p = s.p->data + a, *e = s.p->data + b, *q = p;
    if (p == e) return false;
    bool neg = *q == '-';
    if (*q == '+' || *q == '-') q++;
    if (e - q == 8 && memcmp(q, "Infinity", 8) == 0) {
        *out = neg ? -INFINITY : INFINITY;
        return true;
    }
    size_t nd = 0;
    while (q < e && bm_is_digit(*q)) q++, nd++;
    if (q < e && *q == '.') {
        q++;
        while (q < e && bm_is_digit(*q)) q++, nd++;
    }
    if (nd == 0) return false;
    if (q < e && (*q == 'e' || *q == 'E')) {
        q++;
        if (q < e && (*q == '+' || *q == '-')) q++;
        size_t ne = 0;
        while (q < e && bm_is_digit(*q)) q++, ne++;
        if (ne == 0) return false;
    }
    if (q != e) return false;
    /* The span is validated; strtod stops at e (whitespace, NUL or a non-ASCII byte). */
    char *end;
    double d = strtod(p, &end);
    if (end != e) return false;
    *out = d;
    return true;
}

bool bm_parse_int(bm_str s, bm_int radix, bm_int *out) {
    size_t a, b;
    bm_trim_range(s, true, false, &a, &b);
    const char *p = s.p->data + a, *e = s.p->data + b;
    bool neg = false;
    if (p < e && (*p == '+' || *p == '-')) neg = *p++ == '-';
    bool hex_prefix = e - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    if (radix == 0) {
        radix = 10;
        if (hex_prefix) { radix = 16; p += 2; }
    } else if (radix == 16) {
        if (hex_prefix) p += 2;
    } else if (radix < 2 || radix > 36) {
        return false;
    }
    uint64_t v = 0, limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    size_t nd = 0;
    for (; p < e; p++) {
        char c = *p;
        int dv = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (dv >= radix) break;
        if (v > (limit - (uint64_t)dv) / (uint64_t)radix) return false; /* doesn't fit in int */
        v = v * (uint64_t)radix + (uint64_t)dv;
        nd++;
    }
    if (nd == 0) return false;
    *out = neg ? (bm_int)(0 - v) : (bm_int)v;
    return true;
}

/* ================================================================== arrays */

static int64_t bm_grow_cap(int64_t cur, int64_t min_cap) {
    int64_t c = cur > INT64_MAX / 2 ? INT64_MAX : cur * 2; /* geometric, at least 4 */
    if (c < 4) c = 4;
    return c < min_cap ? min_cap : c;
}

static bm_arrbuf *bm_arrbuf_new(size_t esize, int64_t cap) {
    bm_arrbuf *b = (bm_arrbuf *)bm_alloc(bm_size_mul_add((size_t)cap, esize, sizeof(bm_arrbuf)));
    b->rc = 1;
    b->cap = cap;
    b->_pad[0] = b->_pad[1] = 0;
    return b;
}

/* Copies n elements and retains the copies. */
static void bm_copy_retain(unsigned char *dst, const unsigned char *src, int64_t n, const bm_type *t) {
    size_t es = t->size;
    if (!es || n <= 0) return;
    memcpy(dst, src, (size_t)n * es);
    if (t->retain)
        for (int64_t i = 0; i < n; i++) t->retain(dst + (size_t)i * es);
}

void bm_arr_release_slow(bm_arr a, const bm_type *t) {
    if (t && t->release && t->size) {
        size_t es = t->size;
        for (int64_t i = 0; i < a.len; i++) t->release(a.p->data + (size_t)i * es);
    }
    free(a.p);
}

void bm_arr_make_unique(bm_arr *a, const bm_type *t, bm_int min_cap) {
    bm_arrbuf *p = a->p;
    size_t es = t->size;
    if (p && p->rc == 1) {
        if (p->cap >= min_cap) return;
        int64_t nc = bm_grow_cap(p->cap, min_cap);
        p = (bm_arrbuf *)bm_realloc(p, bm_size_mul_add((size_t)nc, es, sizeof(bm_arrbuf)));
        p->cap = nc;
        a->p = p;
        return;
    }
    if (!p) {
        if (min_cap > 0) a->p = bm_arrbuf_new(es, bm_grow_cap(0, min_cap));
        return;
    }
    /* shared (rc > 1) or immortal (rc < 0): clone the value's elements */
    int64_t len = a->len;
    int64_t nc = min_cap > len ? bm_grow_cap(len, min_cap) : len;
    bm_arrbuf *q = NULL;
    if (nc > 0) {
        q = bm_arrbuf_new(es, nc);
        bm_copy_retain(q->data, p->data, len, t);
    }
    if (p->rc > 0) p->rc--; /* was > 1, so this never frees */
    a->p = q;
}

bm_arr bm_arr_with_capacity(const bm_type *t, bm_int cap) {
    if (cap <= 0) return BM_EMPTY_ARR;
    return (bm_arr){bm_arrbuf_new(t->size, cap), 0};
}

void *bm_arr_reserve_tail(bm_arr *a, const bm_type *t, bm_int n) {
    if (n < 0 || n > INT64_MAX - a->len) bm_trap("invalid array reserve count", NULL);
    bm_arr_make_unique(a, t, a->len + n);
    return a->p ? a->p->data + (size_t)a->len * t->size : NULL;
}

_Noreturn void bm_arr_oob(bm_int i, bm_int len, const char *loc) {
    char msg[96];
    snprintf(msg, sizeof msg, "index %lld out of bounds for length %lld", (long long)i, (long long)len);
    bm_trap(msg, loc);
}

void *bm_arr_at_mut(bm_arr *a, const bm_type *t, bm_int i, const char *loc) {
    bm_int n = bm_arr_len(*a);
    if ((uint64_t)i >= (uint64_t)n) bm_arr_oob(i, n, loc);
    bm_arr_make_unique(a, t, 0);
    return a->p->data + (size_t)i * t->size;
}

void bm_arr_push(bm_arr *a, const bm_type *t, void *elem) {
    int64_t len = bm_arr_len(*a);
    bm_arr_make_unique(a, t, len + 1);
    if (t->size) memcpy(a->p->data + (size_t)len * t->size, elem, t->size);
    a->len = len + 1;
}

bool bm_arr_pop(bm_arr *a, const bm_type *t, void *out) {
    int64_t len = bm_arr_len(*a);
    if (len == 0) return false;
    bm_arr_make_unique(a, t, 0);
    size_t es = t->size;
    if (es) {
        unsigned char *src = a->p->data + (size_t)(len - 1) * es;
        if (out) memcpy(out, src, es);
        else if (t->release) t->release(src);
    }
    a->len = len - 1;
    return true;
}

bool bm_arr_shift(bm_arr *a, const bm_type *t, void *out) {
    int64_t len = bm_arr_len(*a);
    if (len == 0) return false;
    bm_arr_make_unique(a, t, 0);
    size_t es = t->size;
    if (es) {
        unsigned char *d = a->p->data;
        if (out) memcpy(out, d, es);
        else if (t->release) t->release(d);
        memmove(d, d + es, (size_t)(len - 1) * es);
    }
    a->len = len - 1;
    return true;
}

void bm_arr_unshift(bm_arr *a, const bm_type *t, void *elem) {
    int64_t len = bm_arr_len(*a);
    bm_arr_make_unique(a, t, len + 1);
    size_t es = t->size;
    if (es) {
        unsigned char *d = a->p->data;
        memmove(d + es, d, (size_t)len * es);
        memcpy(d, elem, es);
    }
    a->len = len + 1;
}

bm_arr bm_arr_slice(bm_arr a, const bm_type *t, bm_int start, bm_int end, bool has_start, bool has_end) {
    int64_t len = bm_arr_len(a);
    int64_t s = has_start ? bm_clamp_index(start, len) : 0;
    int64_t e = has_end ? bm_clamp_index(end, len) : len;
    if (e <= s) return BM_EMPTY_ARR;
    if (s == 0 && e == len) { bm_arr_retain(a); return a; } /* whole array: share (copy-on-write) */
    bm_arrbuf *b = bm_arrbuf_new(t->size, e - s);
    bm_copy_retain(b->data, a.p->data + (size_t)s * t->size, e - s, t);
    return (bm_arr){b, e - s};
}

bm_arr bm_arr_concat(bm_arr a, bm_arr b, const bm_type *t) {
    int64_t na = bm_arr_len(a), nb = bm_arr_len(b);
    if (nb == 0) { bm_arr_retain(a); return a; }
    if (na == 0) { bm_arr_retain(b); return b; }
    if (na > INT64_MAX - nb) bm_trap("array too long", NULL);
    bm_arrbuf *r = bm_arrbuf_new(t->size, na + nb);
    bm_copy_retain(r->data, a.p->data, na, t);
    bm_copy_retain(r->data + (size_t)na * t->size, b.p->data, nb, t);
    return (bm_arr){r, na + nb};
}

static void bm_swap_bytes(unsigned char *x, unsigned char *y, size_t n) {
    unsigned char tmp[64];
    while (n) {
        size_t c = n < sizeof tmp ? n : sizeof tmp;
        memcpy(tmp, x, c);
        memcpy(x, y, c);
        memcpy(y, tmp, c);
        x += c, y += c, n -= c;
    }
}

void bm_arr_reverse(bm_arr *a, const bm_type *t) {
    int64_t len = bm_arr_len(*a);
    size_t es = t->size;
    if (len < 2 || !es) return;
    bm_arr_make_unique(a, t, 0);
    unsigned char *d = a->p->data;
    for (int64_t i = 0, j = len - 1; i < j; i++, j--) {
        if (es == 8) {
            uint64_t x, y;
            memcpy(&x, d + (size_t)i * 8, 8);
            memcpy(&y, d + (size_t)j * 8, 8);
            memcpy(d + (size_t)i * 8, &y, 8);
            memcpy(d + (size_t)j * 8, &x, 8);
        } else {
            bm_swap_bytes(d + (size_t)i * es, d + (size_t)j * es, es);
        }
    }
}

/* ------------------------------------------------------------------ stable merge sort */

typedef int (*bm_cmp3)(void *ctx, const void *a, const void *b); /* > 0 when a sorts after b */

static void bm_insertion_sort(unsigned char *base, size_t n, size_t es, bm_cmp3 cmp, void *ctx, unsigned char *tmp) {
    for (size_t i = 1; i < n; i++) {
        unsigned char *x = base + i * es;
        if (cmp(ctx, x - es, x) <= 0) continue;
        memcpy(tmp, x, es);
        size_t j = i - 1;
        while (j > 0 && cmp(ctx, base + (j - 1) * es, tmp) > 0) j--;
        memmove(base + (j + 1) * es, base + j * es, (i - j) * es);
        memcpy(base + j * es, tmp, es);
    }
}

/* tmp must hold at least max(1, n/2) elements. */
static void bm_merge_sort(unsigned char *base, size_t n, size_t es, bm_cmp3 cmp, void *ctx, unsigned char *tmp) {
    if (n <= 12) {
        bm_insertion_sort(base, n, es, cmp, ctx, tmp);
        return;
    }
    size_t h = n / 2;
    bm_merge_sort(base, h, es, cmp, ctx, tmp);
    bm_merge_sort(base + h * es, n - h, es, cmp, ctx, tmp);
    if (cmp(ctx, base + (h - 1) * es, base + h * es) <= 0) return; /* already in order */
    memcpy(tmp, base, h * es);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        if (cmp(ctx, tmp + i * es, base + j * es) <= 0) memcpy(base + k++ * es, tmp + i++ * es, es);
        else memcpy(base + k++ * es, base + j++ * es, es);
    }
    if (i < h) memcpy(base + k * es, tmp + i * es, (h - i) * es);
}

typedef struct { double (*cmp)(void *ctx, const void *x, const void *y); void *ctx; } bm_user_cmp;

static int bm_user_cmp_fn(void *c, const void *a, const void *b) {
    bm_user_cmp *u = (bm_user_cmp *)c;
    double r = u->cmp(u->ctx, a, b);
    return r > 0 ? 1 : r < 0 ? -1 : 0; /* NaN counts as 0 (JS) */
}

static int bm_str_cmp_fn(void *c, const void *a, const void *b) {
    (void)c;
    return bm_str_cmp(*(const bm_str *)a, *(const bm_str *)b);
}

typedef struct { bm_str key; int64_t idx; } bm_sort_key;

void bm_arr_sort(bm_arr *a, const bm_type *t, double (*cmp)(void *ctx, const void *x, const void *y), void *ctx) {
    int64_t n = bm_arr_len(*a);
    size_t es = t->size;
    if (n < 2 || !es) return;
    bm_arr_make_unique(a, t, 0);
    unsigned char *d = a->p->data;
    if (cmp || t == &bm_type_str) {
        unsigned char *tmp = (unsigned char *)bm_alloc(bm_size_mul_add((size_t)n / 2 + 1, es, 0));
        bm_user_cmp u = {cmp, ctx};
        if (cmp) bm_merge_sort(d, (size_t)n, es, bm_user_cmp_fn, &u, tmp);
        else bm_merge_sort(d, (size_t)n, es, bm_str_cmp_fn, NULL, tmp);
        free(tmp);
        return;
    }
    /* JS default order: compare String(x). */
    bm_sort_key *keys = (bm_sort_key *)bm_alloc(bm_size_mul_add((size_t)n, sizeof(bm_sort_key), 0));
    for (int64_t i = 0; i < n; i++) {
        bm_sb sb = {0};
        if (t != &bm_type_undefined) t->to_str(&sb, d + (size_t)i * es);
        keys[i].key = bm_str_from_sb(&sb);
        keys[i].idx = i;
    }
    bm_sort_key *tmpk = (bm_sort_key *)bm_alloc(((size_t)n / 2 + 1) * sizeof(bm_sort_key));
    bm_merge_sort((unsigned char *)keys, (size_t)n, sizeof(bm_sort_key), bm_str_cmp_fn, NULL, (unsigned char *)tmpk);
    free(tmpk);
    unsigned char *out = (unsigned char *)bm_alloc((size_t)n * es);
    for (int64_t i = 0; i < n; i++) {
        memcpy(out + (size_t)i * es, d + (size_t)keys[i].idx * es, es);
        bm_str_release(keys[i].key);
    }
    memcpy(d, out, (size_t)n * es);
    free(out);
    free(keys);
}

bm_int bm_arr_index_of(bm_arr a, const bm_type *t, const void *elem) {
    int64_t n = bm_arr_len(a);
    size_t es = t->size;
    for (int64_t i = 0; i < n; i++)
        if (t->eq(a.p->data + (size_t)i * es, elem)) return i;
    return -1;
}

bm_int bm_arr_last_index_of(bm_arr a, const bm_type *t, const void *elem) {
    size_t es = t->size;
    for (int64_t i = bm_arr_len(a) - 1; i >= 0; i--)
        if (t->eq(a.p->data + (size_t)i * es, elem)) return i;
    return -1;
}

static void bm_join_into(bm_sb *sb, bm_arr a, const bm_type *t, const char *sep, size_t sep_len) {
    int64_t n = bm_arr_len(a);
    size_t es = t->size;
    for (int64_t i = 0; i < n; i++) {
        if (i) bm_sb_push(sb, sep, sep_len);
        if (t != &bm_type_undefined) t->to_str(sb, a.p->data + (size_t)i * es);
    }
}

bm_str bm_arr_join(bm_arr a, const bm_type *t, bm_str sep) {
    if (bm_arr_len(a) == 1 && t == &bm_type_str) {
        bm_str s = *(bm_str *)(void *)a.p->data;
        bm_str_retain(s);
        return s;
    }
    bm_sb sb = {0};
    bm_join_into(&sb, a, t, sep.p->data, (size_t)sep.p->len);
    return bm_str_from_sb(&sb);
}

void bm_to_str_arr(bm_sb *sb, bm_arr a, const bm_type *t) { bm_join_into(sb, a, t, ",", 1); }

/* ================================================================== maps and sets */

/* One allocation: header, `cap` entries in insertion order, then 2*cap index slots.
 * Entry: uint64 hash (top bit set; 0 = deleted), key (padded to 8), value (padded to 8).
 * Index slot: entry number + 1, 0 = empty. Open addressing, linear probing, backward-shift
 * deletion (so the index never holds tombstones); deleted entries are compacted away when
 * the entry array fills up. */
struct bm_mapbuf {
    int64_t rc;
    int64_t count;    /* live entries */
    int64_t used;     /* entries used, live or deleted */
    int64_t cap;      /* entry capacity (power of two) */
    uint64_t mask;    /* index slots - 1 */
    uint32_t *index;
    int64_t _pad[2];  /* header = 64 bytes, entries stay 16-byte aligned */
    unsigned char entries[];
};

#define BM_LIVE_BIT ((uint64_t)1 << 63)
#define BM_MAP_MIN_CAP 8

typedef struct { size_t voff, stride; } bm_mlayout;

static inline bm_mlayout bm_mlay(const bm_type *kt, const bm_type *vt) {
    bm_mlayout L;
    L.voff = 8 + ((kt->size + 7) & ~(size_t)7);
    L.stride = L.voff + ((vt->size + 7) & ~(size_t)7);
    return L;
}

static inline unsigned char *bm_entry(const bm_mapbuf *m, bm_mlayout L, int64_t i) {
    return (unsigned char *)m->entries + (size_t)i * L.stride;
}
static inline uint64_t bm_entry_hash(const unsigned char *e) { uint64_t h; memcpy(&h, e, 8); return h; }
static inline void bm_entry_set_hash(unsigned char *e, uint64_t h) { memcpy(e, &h, 8); }

static inline double bm_canon_f64(double x) {
    if (x == 0) return 0.0;  /* -0 → 0 */
    if (x != x) return NAN;  /* one NaN */
    return x;
}

static inline uint64_t bm_key_hash(const bm_type *kt, const void *key) { return kt->hash(key) | BM_LIVE_BIT; }

/* Map keys use SameValueZero: like === except NaN equals NaN. */
static inline bool bm_key_eq(const bm_type *kt, const void *a, const void *b) {
    if (kt == &bm_type_f64) {
        double x = *(const double *)a, y = *(const double *)b;
        return x == y || (x != x && y != y);
    }
    if (kt == &bm_type_f32) {
        float x = *(const float *)a, y = *(const float *)b;
        return x == y || (x != x && y != y);
    }
    return kt->eq(a, b);
}

static bm_mapbuf *bm_mapbuf_new(int64_t cap, bm_mlayout L) {
    if (cap > ((int64_t)1 << 31)) bm_trap("map too large", NULL);
    size_t islots = (size_t)cap * 2;
    size_t bytes = bm_size_mul_add((size_t)cap, L.stride, sizeof(bm_mapbuf));
    bytes = bm_size_mul_add(islots, sizeof(uint32_t), bytes);
    bm_mapbuf *m = (bm_mapbuf *)bm_alloc(bytes);
    m->rc = 1;
    m->count = m->used = 0;
    m->cap = cap;
    m->mask = islots - 1;
    m->index = (uint32_t *)(void *)(m->entries + (size_t)cap * L.stride);
    m->_pad[0] = m->_pad[1] = 0;
    memset(m->index, 0, islots * sizeof(uint32_t));
    return m;
}

/* New buffer with the live entries of `old` (compacted). retain: clone (retain keys and
 * values) rather than move. */
static bm_mapbuf *bm_map_rebuild(const bm_mapbuf *old, bm_mlayout L, int64_t cap, bool retain,
                                 const bm_type *kt, const bm_type *vt) {
    bm_mapbuf *m = bm_mapbuf_new(cap, L);
    int64_t j = 0;
    for (int64_t i = 0; i < old->used; i++) {
        const unsigned char *src = bm_entry(old, L, i);
        uint64_t h = bm_entry_hash(src);
        if (!h) continue;
        unsigned char *dst = bm_entry(m, L, j);
        memcpy(dst, src, L.stride);
        if (retain) {
            if (kt->retain && kt->size) kt->retain(dst + 8);
            if (vt->retain && vt->size) vt->retain(dst + L.voff);
        }
        uint64_t pos = h & m->mask;
        while (m->index[pos]) pos = (pos + 1) & m->mask;
        m->index[pos] = (uint32_t)(j + 1);
        j++;
    }
    m->count = m->used = j;
    return m;
}

static bm_mapbuf *bm_map_unique(bm_map *mp, const bm_type *kt, const bm_type *vt, bm_mlayout L) {
    bm_mapbuf *m = mp->p;
    if (!m) return mp->p = bm_mapbuf_new(BM_MAP_MIN_CAP, L);
    if (m->rc == 1) return m;
    bm_mapbuf *c = bm_map_rebuild(m, L, m->cap, true, kt, vt);
    if (m->rc > 0) m->rc--;
    return mp->p = c;
}

/* Index slot holding `key` (*found = true), or the empty slot where it would go. */
static uint64_t bm_map_find(const bm_mapbuf *m, bm_mlayout L, const bm_type *kt, const void *key, uint64_t h,
                            bool *found) {
    uint64_t i = h & m->mask;
    for (;;) {
        uint32_t e = m->index[i];
        if (!e) { *found = false; return i; }
        const unsigned char *ent = bm_entry(m, L, e - 1);
        if (bm_entry_hash(ent) == h && bm_key_eq(kt, ent + 8, key)) { *found = true; return i; }
        i = (i + 1) & m->mask;
    }
}

void bm_map_retain(bm_map m) {
    if (m.p && m.p->rc >= 0) m.p->rc++;
}

static void bm_map_release_entries(bm_mapbuf *m, const bm_type *kt, const bm_type *vt, bm_mlayout L) {
    bool rk = kt->release && kt->size, rv = vt->release && vt->size;
    if (!rk && !rv) return;
    for (int64_t i = 0; i < m->used; i++) {
        unsigned char *e = bm_entry(m, L, i);
        if (!bm_entry_hash(e)) continue;
        if (rk) kt->release(e + 8);
        if (rv) vt->release(e + L.voff);
    }
}

void bm_map_release(bm_map m, const bm_type *kt, const bm_type *vt) {
    if (!m.p || m.p->rc <= 0 || --m.p->rc > 0) return;
    bm_map_release_entries(m.p, kt, vt, bm_mlay(kt, vt));
    free(m.p);
}

bm_int bm_map_size(bm_map m) { return m.p ? m.p->count : 0; }

void *bm_map_get(bm_map mm, const bm_type *kt, const bm_type *vt, const void *key) {
    bm_mapbuf *m = mm.p;
    if (!m || !m->count) return NULL;
    bm_mlayout L = bm_mlay(kt, vt);
    bool found;
    uint64_t pos = bm_map_find(m, L, kt, key, bm_key_hash(kt, key), &found);
    return found ? bm_entry(m, L, m->index[pos] - 1) + L.voff : NULL;
}

bool bm_map_has(bm_map m, const bm_type *kt, const bm_type *vt, const void *key) {
    return bm_map_get(m, kt, vt, key) != NULL;
}

void bm_map_set(bm_map *mp, const bm_type *kt, const bm_type *vt, void *key, void *value) {
    bm_mlayout L = bm_mlay(kt, vt);
    bm_mapbuf *m = bm_map_unique(mp, kt, vt, L);
    uint64_t h = bm_key_hash(kt, key);
    bool found;
    uint64_t pos = bm_map_find(m, L, kt, key, h, &found);
    if (found) { /* replace the value in place; keep the original key */
        unsigned char *e = bm_entry(m, L, m->index[pos] - 1);
        if (vt->size) {
            if (vt->release) vt->release(e + L.voff);
            memcpy(e + L.voff, value, vt->size);
        }
        if (kt->release && kt->size) kt->release(key);
        return;
    }
    if (m->used == m->cap) {
        /* full: compact if at least half the entries are deleted, else double */
        int64_t ncap = m->count * 2 <= m->cap ? m->cap : m->cap * 2;
        bm_mapbuf *n = bm_map_rebuild(m, L, ncap, false, kt, vt);
        free(m);
        mp->p = m = n;
        pos = h & m->mask;
        while (m->index[pos]) pos = (pos + 1) & m->mask;
    }
    int64_t idx = m->used++;
    unsigned char *e = bm_entry(m, L, idx);
    bm_entry_set_hash(e, h);
    if (kt->size) memcpy(e + 8, key, kt->size);
    if (vt->size) memcpy(e + L.voff, value, vt->size);
    m->index[pos] = (uint32_t)(idx + 1);
    m->count++;
}

bool bm_map_delete(bm_map *mp, const bm_type *kt, const bm_type *vt, const void *key) {
    bm_mapbuf *m = mp->p;
    if (!m || !m->count) return false;
    bm_mlayout L = bm_mlay(kt, vt);
    uint64_t h = bm_key_hash(kt, key);
    bool found;
    uint64_t pos = bm_map_find(m, L, kt, key, h, &found);
    if (!found) return false;
    if (m->rc != 1) {
        m = bm_map_unique(mp, kt, vt, L);
        pos = bm_map_find(m, L, kt, key, h, &found);
    }
    unsigned char *e = bm_entry(m, L, m->index[pos] - 1);
    bm_entry_set_hash(e, 0);
    m->count--;
    /* backward-shift deletion in the index */
    uint64_t mask = m->mask, i = pos, j = pos;
    for (;;) {
        j = (j + 1) & mask;
        uint32_t s = m->index[j];
        if (!s) break;
        uint64_t home = bm_entry_hash(bm_entry(m, L, s - 1)) & mask;
        if (((j - home) & mask) >= ((j - i) & mask)) {
            m->index[i] = s;
            i = j;
        }
    }
    m->index[i] = 0;
    if (m->count == 0) m->used = 0; /* index is empty too */
    /* release last: a release callback must see a consistent map */
    if (kt->release && kt->size) kt->release(e + 8);
    if (vt->release && vt->size) vt->release(e + L.voff);
    return true;
}

void bm_map_clear(bm_map *m, const bm_type *kt, const bm_type *vt) {
    bm_map_release(*m, kt, vt);
    m->p = NULL;
}

static bm_arr bm_map_column(bm_map mm, const bm_type *kt, const bm_type *vt, bool values) {
    bm_mapbuf *m = mm.p;
    if (!m || !m->count) return BM_EMPTY_ARR;
    bm_mlayout L = bm_mlay(kt, vt);
    const bm_type *t = values ? vt : kt;
    size_t off = values ? L.voff : 8, es = t->size;
    bm_arrbuf *b = bm_arrbuf_new(es, m->count);
    int64_t j = 0;
    for (int64_t i = 0; i < m->used; i++) {
        unsigned char *e = bm_entry(m, L, i);
        if (!bm_entry_hash(e)) continue;
        if (es) {
            memcpy(b->data + (size_t)j * es, e + off, es);
            if (t->retain) t->retain(b->data + (size_t)j * es);
        }
        j++;
    }
    return (bm_arr){b, j};
}

bm_arr bm_map_keys(bm_map m, const bm_type *kt, const bm_type *vt) { return bm_map_column(m, kt, vt, false); }
bm_arr bm_map_values(bm_map m, const bm_type *kt, const bm_type *vt) { return bm_map_column(m, kt, vt, true); }

bool bm_map_next(bm_map mm, const bm_type *kt, const bm_type *vt, bm_int *cursor, void **key, void **value) {
    bm_mapbuf *m = mm.p;
    if (!m) return false;
    bm_mlayout L = bm_mlay(kt, vt);
    for (int64_t i = *cursor; i < m->used; i++) {
        unsigned char *e = bm_entry(m, L, i);
        if (!bm_entry_hash(e)) continue;
        if (key) *key = e + 8;
        if (value) *value = e + L.voff;
        *cursor = i + 1;
        return true;
    }
    *cursor = m->used;
    return false;
}

bool bm_map_eq(bm_map a, bm_map b, const bm_type *kt, const bm_type *vt) {
    if (a.p == b.p) return true;
    if (bm_map_size(a) != bm_map_size(b)) return false;
    void *k, *v;
    for (bm_int i = 0; bm_map_next(a, kt, vt, &i, &k, &v);) {
        void *w = bm_map_get(b, kt, vt, k);
        if (!w) return false;
        if (vt->size && !vt->eq(v, w)) return false;
    }
    return true;
}

/* ================================================================== console formatting */

/* A port of the layout rules of Node's util.inspect (lib/internal/util/inspect.js) with the
 * options console.log uses: breakLength 80, compact 3, depth 2, maxArrayLength 100,
 * maxStringLength 10000. A container formats each child into a list of pieces (Node's
 * `output` array), then bm_reduce_to_single_string lays them out on one line or several.
 * The `depth` argument of the inspect functions is Node's `recurseTimes`. */
#define BM_BREAK_LENGTH 80
#define BM_COMPACT 3
#define BM_INSPECT_DEPTH 2
#define BM_MAX_ARRAY_LENGTH 100
#define BM_MAX_STRING_LENGTH 10000
#define BM_MIN_LINE_LENGTH 16

/* Node's ctx.indentationLvl and ctx.currentDepth (single-threaded, reset at depth 0). */
static struct { int64_t indent, current_depth; } bm_ictx;

typedef struct {
    bm_sb text;    /* all pieces, back to back */
    size_t *ends;  /* end offset of each piece */
    size_t n, cap;
} bm_pieces;

static void bm_pieces_mark(bm_pieces *p) { /* ends the current piece */
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->ends = (size_t *)bm_realloc(p->ends, p->cap * sizeof(size_t));
    }
    p->ends[p->n++] = p->text.len;
}

static const char *bm_piece(const bm_pieces *p, size_t i, size_t *len) {
    size_t start = i ? p->ends[i - 1] : 0;
    *len = p->ends[i] - start;
    return p->text.data ? p->text.data + start : "";
}

static void bm_pieces_free(bm_pieces *p) {
    bm_sb_free(&p->text);
    free(p->ends);
}

static void bm_push_spaces(bm_sb *sb, int64_t n) {
    if (n <= 0) return;
    memset(bm_sb_reserve(sb, (size_t)n), ' ', (size_t)n);
    sb->len += (size_t)n;
}

/* JavaScript string length (UTF-16 code units) of UTF-8 text. */
static size_t bm_utf16_len(const char *s, size_t n) {
    size_t u = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        u += (c & 0xC0) != 0x80;
        u += c >= 0xF0; /* surrogate pair */
    }
    return u;
}

/* Code points Node's getStringWidth (ICU) reports as 2 columns wide, generated from Node. */
static const uint32_t bm_wide_ranges[][2] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0},
    {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2630, 0x2637}, {0x2648, 0x2653},
    {0x267F, 0x267F}, {0x268A, 0x268F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB},
    {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705},
    {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x2E99}, {0x2E9B, 0x2EF3}, {0x2F00, 0x2FD5},
    {0x2FF0, 0x303E}, {0x3041, 0x3096}, {0x3099, 0x30FF}, {0x3105, 0x312F}, {0x3131, 0x318E},
    {0x3190, 0x31E5}, {0x31EF, 0x321E}, {0x3220, 0x3247}, {0x3250, 0xA48C}, {0xA490, 0xA4C6},
    {0xA960, 0xA97C}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE52},
    {0xFE54, 0xFE66}, {0xFE68, 0xFE6B}, {0xFF01, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
    {0x16FF0, 0x16FF6}, {0x17000, 0x18CD5}, {0x18CFF, 0x18D1E}, {0x18D80, 0x18DF2}, {0x1AFF0, 0x1AFF3},
    {0x1AFF5, 0x1AFFB}, {0x1AFFD, 0x1AFFE}, {0x1B000, 0x1B122}, {0x1B132, 0x1B132}, {0x1B150, 0x1B152},
    {0x1B155, 0x1B155}, {0x1B164, 0x1B167}, {0x1B170, 0x1B2FB}, {0x1D15E, 0x1D164}, {0x1D1BB, 0x1D1C0},
    {0x1D300, 0x1D356}, {0x1D360, 0x1D376}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F202}, {0x1F210, 0x1F23B}, {0x1F240, 0x1F248}, {0x1F250, 0x1F251},
    {0x1F260, 0x1F265}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D8}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA8A}, {0x1FA8E, 0x1FAC6}, {0x1FAC8, 0x1FAC8},
    {0x1FACD, 0x1FADC}, {0x1FADF, 0x1FAEA}, {0x1FAEF, 0x1FAF8}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

static bool bm_is_wide(uint32_t c) {
    size_t lo = 0, hi = sizeof bm_wide_ranges / sizeof *bm_wide_ranges;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < bm_wide_ranges[mid][0]) hi = mid;
        else if (c > bm_wide_ranges[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

static bool bm_is_zero_width(uint32_t c) { /* Node's isZeroWidthCodePoint */
    return c <= 0x1F || (c >= 0x7F && c <= 0x9F) || (c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) ||
           (c >= 0xE0100 && c <= 0xE01EF);
}

/* Terminal columns (Node's getStringWidth). */
static size_t bm_str_width(const char *s, size_t n) {
    size_t w = 0, i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            w += c >= 32 && c != 0x7F;
            i++;
            continue;
        }
        uint32_t cp;
        i += bm_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        w += bm_is_wide(cp) ? 2 : bm_is_zero_width(cp) ? 0 : 1;
    }
    return w;
}

static bool bm_has_newline(const char *s, size_t n) { return n && memchr(s, '\n', n) != NULL; }

/* Node's strEscape: quote with ' unless the text contains ' (then " if possible, else `
 * if the text has neither ` nor "${"); escape control characters and the backslash. */
static void bm_push_quoted(bm_sb *sb, const char *d, size_t n) {
    char q = '\'';
    if (n && memchr(d, '\'', n)) {
        if (!memchr(d, '"', n)) q = '"';
        else if (!memchr(d, '`', n) && bm_find(d, n, "${", 2, 0) < 0) q = '`';
    }
    bm_sb_push_char(sb, q);
    size_t run = 0; /* start of the pending unescaped run */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        bool c1 = c == 0xC2 && i + 1 < n && (unsigned char)d[i + 1] <= 0x9F; /* U+0080..U+009F */
        if (c >= 0x20 && c != 0x7F && c != '\\' && !(c == '\'' && q == '\'') && !c1) continue;
        bm_sb_push(sb, d + run, i - run);
        char esc[8];
        switch (c) {
        case '\n': bm_sb_push(sb, "\\n", 2); break;
        case '\t': bm_sb_push(sb, "\\t", 2); break;
        case '\r': bm_sb_push(sb, "\\r", 2); break;
        case '\b': bm_sb_push(sb, "\\b", 2); break;
        case '\f': bm_sb_push(sb, "\\f", 2); break;
        case '\\': bm_sb_push(sb, "\\\\", 2); break;
        case '\'': bm_sb_push(sb, "\\'", 2); break;
        default:
            snprintf(esc, sizeof esc, "\\x%02X", c1 ? (unsigned char)d[i + 1] : c);
            bm_sb_push(sb, esc, 4);
            if (c1) i++;
        }
        run = i + 1;
    }
    bm_sb_push(sb, d + run, n - run);
    bm_sb_push_char(sb, q);
}

void bm_inspect_str(bm_sb *sb, bm_str s, int depth) {
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len;
    if (depth == 0) { bm_sb_push(sb, d, n); return; }
    /* formatPrimitive: truncate after maxStringLength code units ... */
    size_t units = bm_utf16_len(d, n), remaining = 0;
    if (units > BM_MAX_STRING_LENGTH) {
        size_t i = 0, u = 0;
        while (i < n) {
            size_t l = bm_utf8_len((unsigned char)d[i]), w = l == 4 ? 2 : 1;
            if (u + w > BM_MAX_STRING_LENGTH) break;
            u += w;
            i += l;
        }
        remaining = units - BM_MAX_STRING_LENGTH;
        units = u;
        n = i;
    }
    /* ... and split long strings after each newline into 'a\n' + 'b' */
    if (units > BM_MIN_LINE_LENGTH && (int64_t)units > BM_BREAK_LENGTH - bm_ictx.indent - 4 && bm_has_newline(d, n)) {
        size_t start = 0;
        while (start < n) {
            const char *nl = (const char *)memchr(d + start, '\n', n - start);
            size_t end = nl ? (size_t)(nl - d) + 1 : n;
            if (start) {
                bm_sb_push(sb, " +\n", 3);
                bm_push_spaces(sb, bm_ictx.indent + 2);
            }
            bm_push_quoted(sb, d + start, end - start);
            start = end;
        }
    } else {
        bm_push_quoted(sb, d, n);
    }
    if (remaining) {
        bm_sb_push_cstr(sb, "... ");
        bm_sb_push_int(sb, (bm_int)remaining);
        bm_sb_push_cstr(sb, remaining > 1 ? " more characters" : " more character");
    }
}

static void bm_push_more_items(bm_pieces *out, int64_t more) {
    bm_sb_push_cstr(&out->text, "... ");
    bm_sb_push_int(&out->text, more);
    bm_sb_push_cstr(&out->text, more > 1 ? " more items" : " more item");
    bm_pieces_mark(out);
}

/* Node's groupArrayElements: lays out > 6 array entries in aligned columns. Returns false
 * (leaving `res` empty) when the entries should not be grouped. */
static bool bm_group_array_elements(const bm_pieces *out, bm_pieces *res, bool numbers) {
    size_t total = 0, max_len = 0, n = out->n, out_len = n, len;
    if (n > BM_MAX_ARRAY_LENGTH) out_len--; /* leave "... more items" out */
    size_t *width = (size_t *)bm_alloc(out_len * 2 * sizeof(size_t)), *units = width + out_len;
    for (size_t i = 0; i < out_len; i++) {
        const char *s = bm_piece(out, i, &len);
        width[i] = bm_str_width(s, len);
        units[i] = bm_utf16_len(s, len);
        total += width[i] + 2;
        if (max_len < width[i]) max_len = width[i];
    }
    int64_t actual_max = (int64_t)max_len + 2; /* + ", " */
    bool grouped = false;
    if (actual_max * 3 + bm_ictx.indent < BM_BREAK_LENGTH &&
        ((double)total / (double)actual_max > 5 || max_len <= 6)) {
        double average_bias = sqrt((double)actual_max - (double)total / (double)n);
        double biased_max = fmax((double)actual_max - 3 - average_bias, 1);
        double c = floor(sqrt(2.5 * biased_max * (double)out_len) / biased_max + 0.5); /* Math.round */
        int64_t columns = (int64_t)c;
        int64_t fit = (BM_BREAK_LENGTH - bm_ictx.indent) / actual_max;
        if (fit < columns) columns = fit;
        if (BM_COMPACT * 4 < columns) columns = BM_COMPACT * 4;
        if (15 < columns) columns = 15;
        if (columns > 1) {
            grouped = true;
            size_t cols = (size_t)columns;
            size_t *line_max = (size_t *)bm_alloc(cols * sizeof(size_t));
            for (size_t i = 0; i < cols; i++) {
                size_t m = 0;
                for (size_t j = i; j < out_len; j += cols)
                    if (width[j] > m) m = width[j];
                line_max[i] = m + 2;
            }
            for (size_t i = 0; i < out_len; i += cols) {
                size_t max = i + cols < out_len ? i + cols : out_len, j = i;
                for (; j < max - 1; j++) { /* "entry, " padded to the column width */
                    const char *s = bm_piece(out, j, &len);
                    int64_t pad = (int64_t)line_max[j - i] - (int64_t)width[j] - 2;
                    if (numbers) bm_push_spaces(&res->text, pad);
                    bm_sb_push(&res->text, s, len);
                    bm_sb_push(&res->text, ", ", 2);
                    if (!numbers) bm_push_spaces(&res->text, pad);
                }
                const char *s = bm_piece(out, j, &len);
                if (numbers) bm_push_spaces(&res->text, (int64_t)line_max[j - i] - (int64_t)width[j] - 2);
                bm_sb_push(&res->text, s, len);
                bm_pieces_mark(res);
            }
            if (out_len < n) {
                const char *s = bm_piece(out, out_len, &len);
                bm_sb_push(&res->text, s, len);
                bm_pieces_mark(res);
            }
            free(line_max);
        }
    }
    free(width);
    return grouped;
}

/* Node's isBelowBreakLength (lengths in UTF-16 code units, like JS .length). */
static bool bm_is_below_break_length(const bm_pieces *out, size_t start) {
    size_t total = out->n + start, len;
    if (total + out->n > BM_BREAK_LENGTH) return false;
    for (size_t i = 0; i < out->n; i++) {
        const char *s = bm_piece(out, i, &len);
        total += bm_utf16_len(s, len);
        if (total > BM_BREAK_LENGTH) return false;
    }
    return true;
}

/* Node's reduceToSingleString (compact = 3, base = ""). Appends to sb and frees out.
 * recurse_times is the container's depth + 1. */
static void bm_reduce_to_single_string(bm_sb *sb, bm_pieces *out, const char *brace0, const char *brace1,
                                       bool is_array, int64_t recurse_times, bool numbers) {
    size_t entries = out->n, len;
    bm_pieces grouped = {0};
    const bm_pieces *o = out;
    if (is_array && entries > 6 && bm_group_array_elements(out, &grouped, numbers)) o = &grouped;
    size_t b0 = strlen(brace0);
    if (bm_ictx.current_depth - recurse_times < BM_COMPACT && entries == o->n) {
        size_t start = o->n + (size_t)bm_ictx.indent + b0 + 10;
        if (bm_is_below_break_length(o, start)) {
            bool newline = false;
            for (size_t i = 0; i < o->n && !newline; i++) {
                const char *s = bm_piece(o, i, &len);
                newline = bm_has_newline(s, len);
            }
            if (!newline) { /* "{ a, b }" */
                bm_sb_push(sb, brace0, b0);
                bm_sb_push_char(sb, ' ');
                for (size_t i = 0; i < o->n; i++) {
                    if (i) bm_sb_push(sb, ", ", 2);
                    const char *s = bm_piece(o, i, &len);
                    bm_sb_push(sb, s, len);
                }
                bm_sb_push_char(sb, ' ');
                bm_sb_push_cstr(sb, brace1);
                goto done;
            }
        }
    }
    /* one entry per line */
    bm_sb_push(sb, brace0, b0);
    for (size_t i = 0; i < o->n; i++) {
        bm_sb_push(sb, i ? ",\n" : "\n", i ? 2 : 1);
        bm_push_spaces(sb, bm_ictx.indent + 2);
        const char *s = bm_piece(o, i, &len);
        bm_sb_push(sb, s, len);
    }
    bm_sb_push_char(sb, '\n');
    bm_push_spaces(sb, bm_ictx.indent);
    bm_sb_push_cstr(sb, brace1);
done:
    bm_pieces_free(&grouped);
    bm_pieces_free(out);
}

/* Common prologue of formatRaw. Returns false when the container was printed as
 * "[Array]"-style because it is deeper than the depth limit. */
static bool bm_inspect_enter(bm_sb *sb, int depth, const char *too_deep) {
    if (depth == 0) bm_ictx.indent = 0; /* top level: recover from an interrupted inspect */
    if (depth > BM_INSPECT_DEPTH) {
        bm_sb_push_cstr(sb, too_deep);
        return false;
    }
    bm_ictx.current_depth = depth + 1;
    return true;
}

static bool bm_is_number_type(const bm_type *t) {
    return t == &bm_type_int || t == &bm_type_f64 || t == &bm_type_f32 || t == &bm_type_i8 || t == &bm_type_i16 ||
           t == &bm_type_i32 || t == &bm_type_u8 || t == &bm_type_u16 || t == &bm_type_u32 || t == &bm_type_u64;
}

void bm_inspect_arr(bm_sb *sb, bm_arr a, const bm_type *t, int depth) {
    int64_t n = bm_arr_len(a);
    if (n == 0) { bm_sb_push(sb, "[]", 2); return; }
    if (!bm_inspect_enter(sb, depth, "[Array]")) return;
    bm_pieces out = {0};
    int64_t shown = n < BM_MAX_ARRAY_LENGTH ? n : BM_MAX_ARRAY_LENGTH;
    for (int64_t i = 0; i < shown; i++) { /* formatProperty: indentation + 2 per entry */
        bm_ictx.indent += 2;
        bm_inspect_value(&out.text, t, a.p->data + (size_t)i * t->size, depth + 1);
        bm_ictx.indent -= 2;
        bm_pieces_mark(&out);
    }
    if (shown < n) bm_push_more_items(&out, n - shown);
    bm_reduce_to_single_string(sb, &out, "[", "]", true, depth + 1, bm_is_number_type(t));
}

static void bm_inspect_map_impl(bm_sb *sb, bm_map m, const bm_type *kt, const bm_type *vt, int depth, bool is_set) {
    int64_t n = bm_map_size(m);
    char brace0[40];
    snprintf(brace0, sizeof brace0, "%s(%lld) {", is_set ? "Set" : "Map", (long long)n);
    if (n == 0) {
        bm_sb_push(sb, brace0, strlen(brace0));
        bm_sb_push_char(sb, '}');
        return;
    }
    if (!bm_inspect_enter(sb, depth, is_set ? "[Set]" : "[Map]")) return;
    bm_pieces out = {0};
    void *k, *v;
    int64_t shown = 0;
    bm_ictx.indent += 2; /* formatSet / formatMap */
    for (bm_int i = 0; shown < BM_MAX_ARRAY_LENGTH && bm_map_next(m, kt, vt, &i, &k, &v); shown++) {
        bm_inspect_value(&out.text, kt, k, depth + 1);
        if (!is_set) {
            bm_sb_push(&out.text, " => ", 4);
            bm_inspect_value(&out.text, vt, v, depth + 1);
        }
        bm_pieces_mark(&out);
    }
    bm_ictx.indent -= 2;
    if (shown < n) bm_push_more_items(&out, n - shown);
    bm_reduce_to_single_string(sb, &out, brace0, "}", false, depth + 1, false);
}

void bm_map_inspect(bm_sb *sb, bm_map m, const bm_type *kt, const bm_type *vt, int depth) {
    bm_inspect_map_impl(sb, m, kt, vt, depth, false);
}

void bm_set_inspect(bm_sb *sb, bm_map m, const bm_type *kt, int depth) {
    bm_inspect_map_impl(sb, m, kt, &bm_type_undefined, depth, true);
}

static bool bm_is_plain_key(const char *s) { /* /^[a-zA-Z_][a-zA-Z_0-9]*$/ */
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) return false;
    for (s++; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_' || (*s >= '0' && *s <= '9')))
            return false;
    return true;
}

void bm_inspect_record(bm_sb *sb, int depth, size_t n, const char *const *names, const bm_type *const *types,
                       const void *const *fields) {
    if (n == 0) { bm_sb_push(sb, "{}", 2); return; }
    if (!bm_inspect_enter(sb, depth, "[Object]")) return;
    bm_pieces out = {0};
    for (size_t i = 0; i < n; i++) { /* formatProperty */
        if (bm_is_plain_key(names[i])) bm_sb_push_cstr(&out.text, names[i]);
        else bm_push_quoted(&out.text, names[i], strlen(names[i]));
        bm_sb_push(&out.text, ": ", 2);
        bm_ictx.indent += 2;
        bm_inspect_value(&out.text, types[i], fields[i], depth + 1);
        bm_ictx.indent -= 2;
        bm_pieces_mark(&out);
    }
    bm_reduce_to_single_string(sb, &out, "{", "}", false, depth + 1, false);
}

void bm_inspect_object(bm_sb *sb, int depth, const char *cls, size_t n, const char *const *names, const bm_type *const *types,
                       const void *const *fields) {
    size_t cl = strlen(cls);
    if (n == 0) {
        bm_sb_push(sb, cls, cl);
        bm_sb_push(sb, " {}", 3);
        return;
    }
    char buf[160];
    snprintf(buf, sizeof buf, "[%s]", cls);
    if (!bm_inspect_enter(sb, depth, buf)) return;
    bm_pieces out = {0};
    for (size_t i = 0; i < n; i++) {
        if (bm_is_plain_key(names[i])) bm_sb_push_cstr(&out.text, names[i]);
        else bm_push_quoted(&out.text, names[i], strlen(names[i]));
        bm_sb_push(&out.text, ": ", 2);
        bm_ictx.indent += 2;
        bm_inspect_value(&out.text, types[i], fields[i], depth + 1);
        bm_ictx.indent -= 2;
        bm_pieces_mark(&out);
    }
    snprintf(buf, sizeof buf, "%s {", cls);
    bm_reduce_to_single_string(sb, &out, buf, "}", false, depth + 1, false);
}

/* ================================================================== primitive type descriptors */

#define BM_DEFINE_INT_TYPE(name, ctype, is_unsigned)                                              \
    static bool bm_##name##_eq(const void *a, const void *b) { return *(const ctype *)a == *(const ctype *)b; } \
    static uint64_t bm_##name##_hash(const void *p) { return bm_mix64((uint64_t)*(const ctype *)p); } \
    static void bm_##name##_to_str(bm_sb *sb, const void *p) {                                    \
        if (is_unsigned) bm_sb_push_u64(sb, (uint64_t)*(const ctype *)p);                          \
        else bm_sb_push_int(sb, (bm_int)*(const ctype *)p);                                        \
    }                                                                                             \
    const bm_type bm_type_##name = {sizeof(ctype), NULL, NULL, bm_##name##_eq, bm_##name##_hash,   \
                                    bm_##name##_to_str, NULL};

BM_DEFINE_INT_TYPE(int, int64_t, 0)
BM_DEFINE_INT_TYPE(i8, int8_t, 0)
BM_DEFINE_INT_TYPE(i16, int16_t, 0)
BM_DEFINE_INT_TYPE(i32, int32_t, 0)
BM_DEFINE_INT_TYPE(u8, uint8_t, 1)
BM_DEFINE_INT_TYPE(u16, uint16_t, 1)
BM_DEFINE_INT_TYPE(u32, uint32_t, 1)
BM_DEFINE_INT_TYPE(u64, uint64_t, 1)
#undef BM_DEFINE_INT_TYPE

static bool bm_f64_eq(const void *a, const void *b) { return *(const double *)a == *(const double *)b; }
static uint64_t bm_f64_hash(const void *p) {
    double x = bm_canon_f64(*(const double *)p);
    uint64_t bits;
    memcpy(&bits, &x, 8);
    return bm_mix64(bits);
}
static void bm_f64_to_str(bm_sb *sb, const void *p) { bm_sb_push_f64(sb, *(const double *)p); }
static void bm_f64_inspect(bm_sb *sb, const void *p, int depth) {
    (void)depth;
    double x = *(const double *)p;
    if (x == 0 && signbit(x)) bm_sb_push(sb, "-0", 2); /* Node shows -0 */
    else bm_sb_push_f64(sb, x);
}
const bm_type bm_type_f64 = {sizeof(double), NULL, NULL, bm_f64_eq, bm_f64_hash, bm_f64_to_str, NULL};

static bool bm_f32_eq(const void *a, const void *b) { return *(const float *)a == *(const float *)b; }
static uint64_t bm_f32_hash(const void *p) { return bm_f64_hash(&(double){(double)*(const float *)p}); }
static void bm_f32_to_str(bm_sb *sb, const void *p) {
    char buf[40];
    bm_sb_push(sb, buf, bm_fmt_number(buf, (double)*(const float *)p, true));
}
static void bm_f32_inspect(bm_sb *sb, const void *p, int depth) {
    (void)depth;
    float x = *(const float *)p;
    if (x == 0 && signbit(x)) bm_sb_push(sb, "-0", 2);
    else bm_f32_to_str(sb, p);
}
const bm_type bm_type_f32 = {sizeof(float), NULL, NULL, bm_f32_eq, bm_f32_hash, bm_f32_to_str, NULL};

static bool bm_bool_eq(const void *a, const void *b) { return *(const bool *)a == *(const bool *)b; }
static uint64_t bm_bool_hash(const void *p) { return *(const bool *)p ? 0x9e3779b97f4a7c15ULL : 0x2545f4914f6cdd1dULL; }
static void bm_bool_to_str(bm_sb *sb, const void *p) { bm_sb_push_cstr(sb, *(const bool *)p ? "true" : "false"); }
const bm_type bm_type_bool = {sizeof(bool), NULL, NULL, bm_bool_eq, bm_bool_hash, bm_bool_to_str, NULL};

static void bm_strp_retain(void *p) { bm_str_retain(*(bm_str *)p); }
static void bm_strp_release(void *p) { bm_str_release(*(bm_str *)p); }
static bool bm_strp_eq(const void *a, const void *b) { return bm_str_eq(*(const bm_str *)a, *(const bm_str *)b); }
static uint64_t bm_strp_hash(const void *p) { return bm_str_hash(*(const bm_str *)p); }
static void bm_strp_to_str(bm_sb *sb, const void *p) { bm_sb_push_str(sb, *(const bm_str *)p); }
const bm_type bm_type_str = {sizeof(bm_str), bm_strp_retain, bm_strp_release, bm_strp_eq, bm_strp_hash,
                             bm_strp_to_str, NULL};

static bool bm_undef_eq(const void *a, const void *b) { (void)a; (void)b; return true; }
static uint64_t bm_undef_hash(const void *p) { (void)p; return 0x6a09e667f3bcc909ULL; }
static void bm_undef_to_str(bm_sb *sb, const void *p) { (void)p; bm_sb_push(sb, "undefined", 9); }
const bm_type bm_type_undefined = {0, NULL, NULL, bm_undef_eq, bm_undef_hash, bm_undef_to_str, NULL};

/* ================================================================== output */

/* Output is buffered in 8 KiB inside the runtime's globals (a program that prints a little
 * touches one page for all of them), moving to 64 KiB once a program fills that. */
enum { BM_OUT_SMALL = 8192, BM_OUT_BIG = 65536 };
static char bm_out_small[BM_OUT_SMALL];
static char *bm_out_big; /* (a pointer initialized to bm_out_small would put a page of data in every binary) */
static size_t bm_out_len;
#define bm_out_buf (bm_out_big ? bm_out_big : bm_out_small)
#define bm_out_cap ((size_t)(bm_out_big ? BM_OUT_BIG : BM_OUT_SMALL))
static bool bm_out_tty; /* stdout is a terminal: flush after every line */

void bm_out_flush(void) {
    if (bm_out_len) {
        bm_write_fd(1, bm_out_buf, bm_out_len);
        bm_out_len = 0;
    }
}

void bm_out_write(const char *s, size_t n) {
    if (!n) return;
    if (n > bm_out_cap - bm_out_len) {
        bm_out_flush();
        if (!bm_out_big) bm_out_big = bm_alloc(BM_OUT_BIG);
        if (n >= bm_out_cap) {
            bm_write_fd(1, s, n);
            return;
        }
    }
    memcpy(bm_out_buf + bm_out_len, s, n);
    bm_out_len += n;
}

void bm_out_sb_line(bm_sb *sb) {
    bm_sb_push_char(sb, '\n');
    bm_out_write(sb->data, sb->len);
    bm_sb_free(sb);
    if (bm_out_tty) bm_out_flush();
}

void bm_err_sb_line(bm_sb *sb) {
    bm_out_flush(); /* keep stdout/stderr ordering */
    bm_sb_push_char(sb, '\n');
    bm_write_fd(2, sb->data, sb->len);
    bm_sb_free(sb);
}

/* ================================================================== math */

double bm_math_round(double x) {
    if (!isfinite(x)) return x;
    double r = floor(x);
    if (x - r >= 0.5) r += 1.0;
    return r == 0 ? copysign(0.0, x) : r; /* Math.round(-0.4) is -0 */
}

bm_int bm_f64_to_int(double x, const char *what, const char *loc) {
    if (BM_LIKELY(x >= -9223372036854775808.0 && x < 9223372036854775808.0)) return (bm_int)x;
    char num[40], msg[160];
    num[bm_fmt_number(num, x, false)] = 0;
    snprintf(msg, sizeof msg, "%s(%s) is %s", what ? what : "int", num,
             x != x ? "not an integer" : "out of the int range");
    bm_trap(msg, loc);
}

/* Stored xor the default seed, so the state is zero-initialized (no __data page in the binary). */
#define BM_RNG_DEFAULT 0x9e3779b97f4a7c15ULL
static uint64_t bm_rng_state;

static void bm_random_seed(uint64_t seed) {
    uint64_t x = bm_mix64(seed + BM_RNG_DEFAULT);
    bm_rng_state = (x ? x : BM_RNG_DEFAULT) ^ BM_RNG_DEFAULT;
}

/* Seeded on first use (BARM_SEED: a number, or any other value for a random seed), so programs
 * that never draw a random number don't link getenv, time and clock. */
static bool bm_rng_ready;
static __attribute__((noinline, cold)) void bm_random_init(void) {
    bm_rng_ready = true;
    const char *seed = getenv("BARM_SEED");
    if (seed) {
        char *end;
        unsigned long long v = strtoull(seed, &end, 0);
        if (end != seed && *end == 0) bm_random_seed(v);
        else bm_random_seed((uint64_t)time(NULL) ^ ((uint64_t)clock() << 32) ^ (uint64_t)(uintptr_t)&seed);
    } else {
        bm_random_seed(0);
    }
}

double bm_random(void) { /* xorshift64* */
    if (BM_UNLIKELY(!bm_rng_ready)) bm_random_init();
    uint64_t x = bm_rng_state ^ BM_RNG_DEFAULT;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    bm_rng_state = x ^ BM_RNG_DEFAULT;
    return (double)((x * 0x2545f4914f6cdd1dULL) >> 11) * 0x1.0p-53;
}

/* ================================================================== closures */

void bm_env_release_slow(bm_env *e) {
    if (e->drop) e->drop(e);
    free(e);
}

/* ================================================================== tests */

bool bm_test_active(void) { return bm_test_jmp != NULL; }

static void bm_test_longjmp(void) { longjmp(*bm_test_jmp, 1); }

void bm_test_run(const char *name, void (*fn)(void)) {
    jmp_buf jb;
    bm_test_jmp = &jb;
    bm_test_unwind = bm_test_longjmp;
    bm_sb sb = {0};
    if (setjmp(jb) == 0) {
        fn();
        bm_test_jmp = NULL;
        bm_tests_passed++;
        bm_sb_push_cstr(&sb, "ok   ");
        bm_sb_push_cstr(&sb, name);
    } else {
        bm_test_jmp = NULL;
        bm_tests_failed++;
        bm_sb_push_cstr(&sb, "FAIL ");
        bm_sb_push_cstr(&sb, name);
        bm_sb_push_cstr(&sb, "\n  ");
        bm_sb_push(&sb, bm_test_msg.data, bm_test_msg.len);
        if (bm_test_loc && *bm_test_loc) {
            bm_sb_push_cstr(&sb, "\n  at ");
            bm_sb_push_cstr(&sb, bm_test_loc);
        }
        bm_sb_free(&bm_test_msg);
        bm_test_loc = NULL;
    }
    bm_out_sb_line(&sb);
}

_Noreturn void bm_expect_fail(bm_sb *message, const char *loc) {
    bm_sb msg = {0};
    if (message) {
        msg = *message;
        *message = (bm_sb){0};
    }
    if (!msg.len) bm_sb_push_cstr(&msg, "expectation failed");
    if (bm_test_jmp) {
        bm_sb_free(&bm_test_msg);
        bm_test_msg = msg;
        bm_test_loc = loc;
        bm_test_unwind();
    }
    bm_sb_push_char(&msg, 0);
    bm_trap(msg.data, loc);
}

int bm_test_summary(void) {
    bm_sb sb = {0};
    bm_sb_push_int(&sb, bm_tests_passed);
    bm_sb_push_cstr(&sb, " passed, ");
    bm_sb_push_int(&sb, bm_tests_failed);
    bm_sb_push_cstr(&sb, " failed");
    bm_out_sb_line(&sb);
    return bm_tests_failed ? 1 : 0;
}

/* ================================================================== init */

void bm_init(int argc, char **argv) {
    static bool initialized;
    bm_argc = argc;
    bm_argv = argv;
    if (initialized) return;
    initialized = true;
    atexit(bm_out_flush);
#ifdef BM_HAVE_ISATTY
    bm_out_tty = isatty(1) != 0;
#endif
}

/* ================================================================== system */

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

static bm_sb bm_native_err;

static const char *bm_errno_name(int e) {
    switch (e) {
    case ENOENT: return "ENOENT";
    case EACCES: return "EACCES";
    case EEXIST: return "EEXIST";
    case ENOTDIR: return "ENOTDIR";
    case EISDIR: return "EISDIR";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EPERM: return "EPERM";
    case EBUSY: return "EBUSY";
    case EMFILE: return "EMFILE";
    case ENOSPC: return "ENOSPC";
    case EROFS: return "EROFS";
    default: return "EIO";
    }
}

static const char *bm_errno_text(int e) {
    switch (e) {
    case ENOENT: return "no such file or directory";
    case EACCES: return "permission denied";
    case EEXIST: return "file already exists";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "illegal operation on a directory";
    case ENOTEMPTY: return "directory not empty";
    case EPERM: return "operation not permitted";
    default: return strerror(e);
    }
}

/* "ENOENT: no such file or directory, open 'path'" */
static void bm_native_fail(int e, const char *op, bm_str path) {
    if (bm_native_err.len) return;
    bm_sb_push_cstr(&bm_native_err, bm_errno_name(e));
    bm_sb_push_cstr(&bm_native_err, ": ");
    bm_sb_push_cstr(&bm_native_err, bm_errno_text(e));
    bm_sb_push_cstr(&bm_native_err, ", ");
    bm_sb_push_cstr(&bm_native_err, op);
    bm_sb_push_cstr(&bm_native_err, " '");
    bm_sb_push(&bm_native_err, path.p->data, (size_t)path.p->len);
    bm_sb_push_char(&bm_native_err, '\'');
}

bm_str bm_native_takeError(void) {
    if (!bm_native_err.len) return BM_EMPTY_STR;
    return bm_str_from_sb(&bm_native_err);
}

bm_str bm_native_readFile(bm_str path) {
    FILE *f = fopen(path.p->data, "rb");
    if (!f) { bm_native_fail(errno, "open", path); return BM_EMPTY_STR; }
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && S_ISDIR(st.st_mode)) { fclose(f); bm_native_fail(EISDIR, "read", path); return BM_EMPTY_STR; }
    bm_sb sb = {0};
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) bm_sb_push(&sb, buf, n);
    int err = ferror(f) ? errno : 0;
    fclose(f);
    if (err) { bm_sb_free(&sb); bm_native_fail(err, "read", path); return BM_EMPTY_STR; }
    return bm_str_from_sb(&sb);
}

void bm_native_writeFile(bm_str path, bm_str data, bool append) {
    FILE *f = fopen(path.p->data, append ? "ab" : "wb");
    if (!f) { bm_native_fail(errno, "open", path); return; }
    if (data.p->len && fwrite(data.p->data, 1, (size_t)data.p->len, f) != (size_t)data.p->len) bm_native_fail(errno, "write", path);
    if (fclose(f) != 0) bm_native_fail(errno, "close", path);
}

bool bm_native_exists(bm_str path) {
    struct stat st;
    return stat(path.p->data, &st) == 0;
}

static int bm_cmp_cstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

bm_arr bm_native_readDir(bm_str path) {
    DIR *d = opendir(path.p->data);
    if (!d) { bm_native_fail(errno, "scandir", path); return BM_EMPTY_ARR; }
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = bm_realloc(names, cap * sizeof(char *)); }
        size_t len = strlen(e->d_name);
        names[n] = bm_alloc(len + 1);
        memcpy(names[n], e->d_name, len + 1);
        n++;
    }
    closedir(d);
    qsort(names, n, sizeof(char *), bm_cmp_cstr);
    bm_arr out = bm_arr_with_capacity(&bm_type_str, (bm_int)n);
    for (size_t i = 0; i < n; i++) {
        bm_str s = bm_str_from(names[i], strlen(names[i]));
        bm_arr_push(&out, &bm_type_str, &s);
        bm_free(names[i]);
    }
    bm_free(names);
    return out;
}

static int bm_mkdir_p(char *p) {
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, 0777) != 0 && errno != EEXIST) { *s = '/'; return -1; }
        *s = '/';
    }
    if (mkdir(p, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

void bm_native_mkdir(bm_str path, bool recursive) {
    if (recursive) {
        char *p = bm_alloc((size_t)path.p->len + 1);
        memcpy(p, path.p->data, (size_t)path.p->len + 1);
        if (bm_mkdir_p(p) != 0) bm_native_fail(errno, "mkdir", path);
        bm_free(p);
        return;
    }
    if (mkdir(path.p->data, 0777) != 0) bm_native_fail(errno, "mkdir", path);
}

void bm_native_unlink(bm_str path) {
    if (unlink(path.p->data) != 0) bm_native_fail(errno, "unlink", path);
}

static int bm_rm_rf(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (!d) return -1;
        struct dirent *e;
        size_t pl = strlen(p);
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            size_t nl = strlen(e->d_name);
            char *child = bm_alloc(pl + nl + 2);
            memcpy(child, p, pl);
            child[pl] = '/';
            memcpy(child + pl + 1, e->d_name, nl + 1);
            int r = bm_rm_rf(child);
            bm_free(child);
            if (r != 0) { closedir(d); return -1; }
        }
        closedir(d);
        return rmdir(p);
    }
    return unlink(p);
}

void bm_native_rm(bm_str path, bool recursive, bool force) {
    struct stat st;
    if (lstat(path.p->data, &st) != 0) {
        if (!(force && errno == ENOENT)) bm_native_fail(errno, "rm", path);
        return;
    }
    if (S_ISDIR(st.st_mode) && !recursive) { bm_native_fail(EISDIR, "rm", path); return; }
    if (bm_rm_rf(path.p->data) != 0) bm_native_fail(errno, "rm", path);
}

bm_str bm_native_cwd(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) return bm_str_from(".", 1);
    return bm_str_from(buf, strlen(buf));
}

bm_arr bm_process_argv(void) {
    bm_arr out = bm_arr_with_capacity(&bm_type_str, bm_argc + 1);
    /* Node: [node binary, script, args...]; a Barm program is both, so argv[0] appears twice. */
    for (int i = -1; i < bm_argc; i++) {
        const char *a = bm_argv[i < 0 ? 0 : i];
        bm_str s = bm_str_from(a, strlen(a));
        bm_arr_push(&out, &bm_type_str, &s);
    }
    return out;
}

bool bm_process_env(bm_str name, bm_str *out) {
    const char *v = getenv(name.p->data);
    if (!v) return false;
    *out = bm_str_from(v, strlen(v));
    return true;
}

_Noreturn void bm_process_exit(bm_int code) {
    bm_out_flush();
    exit((int)code);
}

double bm_date_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
}

static struct timespec bm_start_time;
double bm_performance_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (bm_start_time.tv_sec == 0 && bm_start_time.tv_nsec == 0) bm_start_time = ts;
    return (double)(ts.tv_sec - bm_start_time.tv_sec) * 1e3 + (double)(ts.tv_nsec - bm_start_time.tv_nsec) / 1e6;
}

void bm_write_stdout(bm_str s) { bm_out_write(s.p->data, (size_t)s.p->len); }

void bm_write_stderr(bm_str s) {
    bm_out_flush();
    bm_write_fd(2, s.p->data, (size_t)s.p->len);
}

/* ================================================================== JSON */

/* Bytes that need escaping in a JSON string: control characters, '"' and '\\'. */
static const uint8_t bm_json_esc[256] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
};

void bm_json_quote(bm_sb *sb, bm_str s) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *d = (const unsigned char *)s.p->data;
    size_t n = (size_t)s.p->len, i = 0;
    bm_sb_reserve(sb, n + 2);
    sb->data[sb->len++] = '"';
    while (i < n) {
        size_t start = i;
        while (i < n && !bm_json_esc[d[i]]) i++;
        if (i > start) bm_sb_push(sb, (const char *)d + start, i - start);
        if (i == n) break;
        unsigned char c = d[i++];
        char buf[6];
        switch (c) {
        case '"': bm_sb_push(sb, "\\\"", 2); break;
        case '\\': bm_sb_push(sb, "\\\\", 2); break;
        case '\b': bm_sb_push(sb, "\\b", 2); break;
        case '\f': bm_sb_push(sb, "\\f", 2); break;
        case '\n': bm_sb_push(sb, "\\n", 2); break;
        case '\r': bm_sb_push(sb, "\\r", 2); break;
        case '\t': bm_sb_push(sb, "\\t", 2); break;
        default:
            buf[0] = '\\'; buf[1] = 'u'; buf[2] = '0'; buf[3] = '0'; buf[4] = hex[c >> 4]; buf[5] = hex[c & 15];
            bm_sb_push(sb, buf, 6);
        }
    }
    bm_sb_push_char(sb, '"');
}

void bm_json_number(bm_sb *sb, double x) {
    if (x != x || x == INFINITY || x == -INFINITY) { bm_sb_push_cstr(sb, "null"); return; }
    if (x == 0) { bm_sb_push_char(sb, '0'); return; } /* -0 prints as 0 */
    bm_sb_push_f64(sb, x);
}

void bm_jp_init(bm_jp *p, bm_str text) {
    p->s = (const unsigned char *)text.p->data;
    p->n = (size_t)text.p->len;
    p->i = 0;
    p->err[0] = 0;
}

static void bm_jp_ws(bm_jp *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
}

char bm_jp_peek(bm_jp *p) {
    bm_jp_ws(p);
    return p->i < p->n ? (char)p->s[p->i] : 0;
}

bool bm_jp_fail(bm_jp *p, const char *expected) {
    if (p->err[0]) return false;
    if (p->i >= p->n) snprintf(p->err, sizeof p->err, "Unexpected end of JSON input (expected %s)", expected);
    else if (p->s[p->i] < 0x20 || p->s[p->i] >= 0x7f) snprintf(p->err, sizeof p->err, "Expected %s at position %zu of the JSON input", expected, p->i);
    else snprintf(p->err, sizeof p->err, "Expected %s but found '%c' at position %zu of the JSON input", expected, p->s[p->i], p->i);
    return false;
}

bool bm_jp_char(bm_jp *p, char c) {
    if (bm_jp_peek(p) == c) { p->i++; return true; }
    char what[4] = { '\'', c, '\'', 0 };
    return bm_jp_fail(p, what);
}

bool bm_jp_try_char(bm_jp *p, char c) {
    if (bm_jp_peek(p) == c) { p->i++; return true; }
    return false;
}

bool bm_jp_word(bm_jp *p, const char *w) {
    bm_jp_ws(p);
    size_t n = strlen(w);
    if (p->n - p->i >= n && memcmp(p->s + p->i, w, n) == 0) { p->i += n; return true; }
    return bm_jp_fail(p, w);
}

static int bm_jp_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void bm_sb_push_utf8(bm_sb *sb, uint32_t cp) {
    char b[4];
    if (cp < 0x80) { b[0] = (char)cp; bm_sb_push(sb, b, 1); }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); bm_sb_push(sb, b, 2); }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); bm_sb_push(sb, b, 3); }
    else { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); bm_sb_push(sb, b, 4); }
}

static bool bm_jp_u4(bm_jp *p, uint32_t *out) {
    if (p->n - p->i < 4) return bm_jp_fail(p, "4 hex digits");
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = bm_jp_hex(p->s[p->i + k]);
        if (h < 0) return bm_jp_fail(p, "a hex digit");
        v = v * 16 + (uint32_t)h;
    }
    p->i += 4;
    *out = v;
    return true;
}

bool bm_jp_string(bm_jp *p, bm_str *out) {
    if (bm_jp_peek(p) != '"') return bm_jp_fail(p, "a string");
    p->i++;
    size_t start = p->i;
    /* Fast path: no escapes. */
    while (p->i < p->n && p->s[p->i] != '"' && p->s[p->i] != '\\' && p->s[p->i] >= 0x20) p->i++;
    if (p->i < p->n && p->s[p->i] == '"') {
        *out = bm_str_from((const char *)p->s + start, p->i - start);
        p->i++;
        return true;
    }
    bm_sb sb = {0};
    bm_sb_push(&sb, (const char *)p->s + start, p->i - start);
    while (p->i < p->n) {
        unsigned char c = p->s[p->i];
        if (c == '"') { p->i++; *out = bm_str_from_sb(&sb); return true; }
        if (c < 0x20) { bm_sb_free(&sb); return bm_jp_fail(p, "a string character"); }
        if (c != '\\') { bm_sb_push_char(&sb, (char)c); p->i++; continue; }
        p->i++;
        if (p->i >= p->n) break;
        char e = (char)p->s[p->i++];
        switch (e) {
        case '"': bm_sb_push_char(&sb, '"'); break;
        case '\\': bm_sb_push_char(&sb, '\\'); break;
        case '/': bm_sb_push_char(&sb, '/'); break;
        case 'b': bm_sb_push_char(&sb, '\b'); break;
        case 'f': bm_sb_push_char(&sb, '\f'); break;
        case 'n': bm_sb_push_char(&sb, '\n'); break;
        case 'r': bm_sb_push_char(&sb, '\r'); break;
        case 't': bm_sb_push_char(&sb, '\t'); break;
        case 'u': {
            uint32_t cp;
            if (!bm_jp_u4(p, &cp)) { bm_sb_free(&sb); return false; }
            if (cp >= 0xD800 && cp < 0xDC00 && p->n - p->i >= 6 && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                p->i += 2;
                uint32_t lo;
                if (!bm_jp_u4(p, &lo)) { bm_sb_free(&sb); return false; }
                if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else { bm_sb_push_utf8(&sb, 0xFFFD); cp = lo; }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
            bm_sb_push_utf8(&sb, cp);
            break;
        }
        default: bm_sb_free(&sb); p->i--; return bm_jp_fail(p, "a valid escape");
        }
    }
    bm_sb_free(&sb);
    return bm_jp_fail(p, "'\"'");
}

bool bm_jp_number(bm_jp *p, double *out) {
    bm_jp_ws(p);
    size_t start = p->i, i = p->i;
    if (i < p->n && p->s[i] == '-') i++;
    if (i < p->n && p->s[i] == '0') i++;
    else if (i < p->n && p->s[i] >= '1' && p->s[i] <= '9') { while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') i++; }
    else return bm_jp_fail(p, "a number");
    if (i < p->n && p->s[i] == '.') {
        i++;
        if (!(i < p->n && p->s[i] >= '0' && p->s[i] <= '9')) { p->i = i; return bm_jp_fail(p, "a digit"); }
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') i++;
    }
    if (i < p->n && (p->s[i] == 'e' || p->s[i] == 'E')) {
        i++;
        if (i < p->n && (p->s[i] == '+' || p->s[i] == '-')) i++;
        if (!(i < p->n && p->s[i] >= '0' && p->s[i] <= '9')) { p->i = i; return bm_jp_fail(p, "a digit"); }
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') i++;
    }
    /* Fast path (exact): up to 15 significant digits, no exponent part, few decimals:
     * mantissa / 10^decimals is correctly rounded because both are exact doubles. */
    {
        uint64_t mant = 0;
        int digits = 0, decimals = 0;
        bool neg = false, simple = true, frac = false;
        for (size_t j = start; j < i; j++) {
            unsigned char c = p->s[j];
            if (c == '-') neg = true;
            else if (c == '.') frac = true;
            else if (c >= '0' && c <= '9') {
                if (mant || c != '0') digits++;
                mant = mant * 10 + (uint64_t)(c - '0');
                if (frac) decimals++;
            } else { simple = false; break; }
        }
        static const double p10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
        if (simple && digits <= 15 && decimals <= 22) {
            double x = (double)mant / p10[decimals];
            *out = neg ? -x : x;
            p->i = i;
            return true;
        }
    }
    char buf[64];
    size_t len = i - start;
    char *tmp = len < sizeof buf ? buf : bm_alloc(len + 1);
    memcpy(tmp, p->s + start, len);
    tmp[len] = 0;
    *out = strtod(tmp, NULL);
    if (tmp != buf) bm_free(tmp);
    p->i = i;
    return true;
}

bool bm_jp_skip(bm_jp *p) {
    char c = bm_jp_peek(p);
    switch (c) {
    case '"': { bm_str s; if (!bm_jp_string(p, &s)) return false; bm_str_release(s); return true; }
    case '{':
        p->i++;
        if (bm_jp_try_char(p, '}')) return true;
        do {
            bm_str k;
            if (!bm_jp_string(p, &k)) return false;
            bm_str_release(k);
            if (!bm_jp_char(p, ':') || !bm_jp_skip(p)) return false;
        } while (bm_jp_try_char(p, ','));
        return bm_jp_char(p, '}');
    case '[':
        p->i++;
        if (bm_jp_try_char(p, ']')) return true;
        do {
            if (!bm_jp_skip(p)) return false;
        } while (bm_jp_try_char(p, ','));
        return bm_jp_char(p, ']');
    case 't': return bm_jp_word(p, "true");
    case 'f': return bm_jp_word(p, "false");
    case 'n': return bm_jp_word(p, "null");
    default: { double d; return bm_jp_number(p, &d); }
    }
}

bool bm_jp_end(bm_jp *p) {
    if (bm_jp_peek(p) == 0) return true;
    return bm_jp_fail(p, "the end of the input");
}

bool bm_jp_find_key(bm_jp *p, const char *key, bm_str *val) {
    size_t saved = p->i;
    char saved_err[sizeof p->err];
    memcpy(saved_err, p->err, sizeof p->err);
    bool found = false;
    if (bm_jp_try_char(p, '{') && !bm_jp_try_char(p, '}')) {
        do {
            bm_str k;
            if (!bm_jp_string(p, &k) || !bm_jp_char(p, ':')) break;
            bool match = strcmp(k.p->data, key) == 0;
            bm_str_release(k);
            if (match && bm_jp_peek(p) == '"') { found = bm_jp_string(p, val); break; }
            if (!bm_jp_skip(p)) break;
        } while (bm_jp_try_char(p, ','));
    }
    p->i = saved;
    memcpy(p->err, saved_err, sizeof p->err);
    return found;
}

bm_str bm_jp_error(bm_jp *p) {
    const char *e = p->err[0] ? p->err : "Invalid JSON";
    return bm_str_from(e, strlen(e));
}

/* ================================================================== async
 * Tasks, promises, the microtask queue and timers (see barm.h). */

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#endif

bm_task *bm_cur_task;
static void (*bm_err_retain)(void *);
static void (*bm_err_release)(void *);

/* Tasks, promises and pending HTTP requests come and go per request: per-size free lists
 * (32-byte classes up to 2 KiB) refilled from 64 KiB slabs; bigger ones from malloc. */
enum { BM_ASYNC_STEP = 32, BM_ASYNC_MAX = 2048, BM_ASYNC_CLASSES = BM_ASYNC_MAX / BM_ASYNC_STEP + 1 };
static void *bm_async_bins[BM_ASYNC_CLASSES];
static bm_class_space bm_async_space[BM_ASYNC_CLASSES];

static void *bm_async_alloc(size_t size) {
#ifdef BM_PLAIN_ALLOC
    return bm_alloc(size);
#endif
    if (size > BM_ASYNC_MAX) return bm_alloc(size);
    size_t c = (size + BM_ASYNC_STEP - 1) / BM_ASYNC_STEP;
    void **f = bm_async_bins[c];
    if (BM_LIKELY(f != NULL)) {
        bm_async_bins[c] = *f;
        return f;
    }
    return bm_carve(&bm_async_space[c], c * BM_ASYNC_STEP);
}

static void bm_async_free(void *p, size_t size) {
#ifdef BM_PLAIN_ALLOC
    free(p);
    return;
#endif
    if (size > BM_ASYNC_MAX) {
        free(p);
        return;
    }
    size_t c = (size + BM_ASYNC_STEP - 1) / BM_ASYNC_STEP;
    *(void **)p = bm_async_bins[c];
    bm_async_bins[c] = p;
}
static void (*bm_err_report)(void *);

void bm_async_init(void (*err_retain)(void *), void (*err_release)(void *), void (*err_report)(void *)) {
    bm_err_retain = err_retain;
    bm_err_release = err_release;
    bm_err_report = err_report;
}

/* ---------------------------------------------------------------- microtask queue (a ring) */

typedef struct { void (*fn)(void *, void *); void *a, *b; } bm_job;
static bm_job *bm_mq;
static size_t bm_mq_head, bm_mq_cap;
size_t bm_mq_len;

static void bm_mq_push(void (*fn)(void *, void *), void *a, void *b) {
    if (bm_mq_len == bm_mq_cap) {
        size_t cap = bm_mq_cap ? bm_mq_cap * 2 : 64;
        bm_job *q = bm_alloc(cap * sizeof *q);
        for (size_t i = 0; i < bm_mq_len; i++) q[i] = bm_mq[(bm_mq_head + i) & (bm_mq_cap - 1)];
        bm_free(bm_mq);
        bm_mq = q;
        bm_mq_head = 0;
        bm_mq_cap = cap;
    }
    bm_mq[(bm_mq_head + bm_mq_len) & (bm_mq_cap - 1)] = (bm_job){fn, a, b};
    bm_mq_len++;
}

/* Rejections nobody awaited by the time the microtask queue drained (reported, then exit 1). */
static bm_promise **bm_unhandled;
static size_t bm_nunhandled, bm_unhandled_cap;

static void bm_check_unhandled(void) {
    for (size_t i = 0; i < bm_nunhandled; i++) {
        bm_promise *p = bm_unhandled[i];
        if (!p->handled) {
            bm_out_flush();
            if (bm_err_report) bm_err_report(p->err);
            exit(1);
        }
        p->reported = false;
        bm_promise_release(p);
    }
    bm_nunhandled = 0;
}

static void bm_run_microtasks(void) {
    while (bm_mq_len) {
        bm_job j = bm_mq[bm_mq_head];
        bm_mq_head = (bm_mq_head + 1) & (bm_mq_cap - 1);
        bm_mq_len--;
        j.fn(j.a, j.b);
    }
    if (bm_nunhandled) bm_check_unhandled();
}

static void bm_callback_job(void *fn, void *env) {
    ((void (*)(bm_env *))fn)((bm_env *)env);
    bm_env_release((bm_env *)env);
}

void bm_queue_microtask(bm_fn cb) {
    bm_env_retain(cb.env);
    bm_mq_push(bm_callback_job, cb.fn, cb.env);
}

/* ---------------------------------------------------------------- tasks */

static void bm_resume_job(void *a, void *b);

bm_task *bm_task_new(size_t frame_size, bm_task_run run, const bm_type *vt) {
    bm_task *t = bm_async_alloc(sizeof(bm_task) + frame_size);
    memset(t, 0, sizeof(bm_task) + frame_size);
    t->run = run;
    t->size = (uint32_t)frame_size;
    t->promise = bm_promise_new(vt);
    return t;
}

/* Runs the task as the current one; frees it once it has finished. */
static void bm_task_step(bm_task *t) {
    bm_task *prev = bm_cur_task;
    bm_cur_task = t;
    bool done = t->run(t);
    bm_cur_task = prev;
    if (done) {
        bm_promise_release(t->promise);
        bm_async_free(t, sizeof(bm_task) + t->size);
    }
}

static void bm_resume_job(void *a, void *b) {
    (void)b;
    bm_task *t = a;
    t->flags &= ~BM_TASK_SYNC;   /* run from the queue: nothing of its starter is left to run */
    bm_task_step(t);
}

/* Set by the standard library right before starting a task in tail position: nothing observable
 * runs between the start and the next microtask checkpoint, so it may continue eagerly. */
static bool bm_spawn_tail;

void bm_native_spawnTail(void) { bm_spawn_tail = true; }

bm_promise *bm_task_spawn(bm_task *t) {
    bm_promise *p = t->promise;
    bm_promise_retain(p);
    if (bm_spawn_tail) bm_spawn_tail = false;
    else t->flags |= BM_TASK_SYNC;
    bm_task_step(t);
    return p;
}

void bm_task_start(bm_task *t) { bm_task_step(t); }

void bm_task_yield(void) {
    if (!bm_cur_task) bm_trap("internal error: await outside a task", NULL);
    bm_mq_push(bm_resume_job, bm_cur_task, NULL);
}

/* ---------------------------------------------------------------- promises */

bm_promise *bm_promise_new(const bm_type *vt) {
    size_t size = vt ? vt->size : 0;
    bm_promise *p = bm_async_alloc(sizeof(bm_promise) + size);
    memset(p, 0, sizeof(bm_promise));
    p->rc = 1;
    p->vt = vt;
    return p;
}

void bm_promise_release_slow(bm_promise *p) {
    if (p->state == BM_FULFILLED && p->vt && p->vt->release) p->vt->release(p->value);
    if (p->state == BM_REJECTED && p->err && bm_err_release) bm_err_release(p->err);
    bm_free(p->more);
    bm_async_free(p, sizeof(bm_promise) + (p->vt ? p->vt->size : 0));
}

static void bm_promise_wake(bm_promise *p) {
    if (p->waiter) {
        bm_mq_push(bm_resume_job, p->waiter, NULL);
        p->waiter = NULL;
    }
    for (int32_t i = 0; i < p->nmore; i++) bm_mq_push(p->more[i].fn, p->more[i].a, p->more[i].b);
    p->nmore = 0;
}

/* Runs fn(a, b) as a microtask once p settles (at once if it has), after earlier reactions. */
static void bm_promise_react(bm_promise *p, void (*fn)(void *, void *), void *a, void *b) {
    p->handled = true;
    if (p->state != BM_PENDING) {
        bm_mq_push(fn, a, b);
        return;
    }
    if (p->nmore == p->capmore) {
        p->capmore = p->capmore ? p->capmore * 2 : 4;
        p->more = bm_realloc(p->more, (size_t)p->capmore * sizeof *p->more);
    }
    p->more[p->nmore++] = (bm_reaction){fn, a, b};
}

void bm_promise_resolve(bm_promise *p, const void *value) {
    if (p->state != BM_PENDING) return;
    size_t size = p->vt ? p->vt->size : 0;
    if (size) {
        memcpy(p->value, value, size);
        if (p->vt->retain) p->vt->retain(p->value);
    }
    p->state = BM_FULFILLED;
    bm_promise_wake(p);
}

void bm_promise_resolve_move(bm_promise *p, void *value) {
    if (p->state != BM_PENDING) {
        if (p->vt && p->vt->release) p->vt->release(value);
        return;
    }
    size_t size = p->vt ? p->vt->size : 0;
    if (size) memcpy(p->value, value, size);
    p->state = BM_FULFILLED;
    bm_promise_wake(p);
}

void bm_promise_reject(bm_promise *p, void *err) {
    if (p->state != BM_PENDING) {
        if (bm_err_release) bm_err_release(err);
        return;
    }
    p->err = err;
    p->state = BM_REJECTED;
    if (!p->handled && !p->waiter && !p->reported) {
        if (bm_nunhandled == bm_unhandled_cap) {
            bm_unhandled_cap = bm_unhandled_cap ? bm_unhandled_cap * 2 : 8;
            bm_unhandled = bm_realloc(bm_unhandled, bm_unhandled_cap * sizeof *bm_unhandled);
        }
        p->reported = true;
        bm_promise_retain(p);
        bm_unhandled[bm_nunhandled++] = p;
    }
    bm_promise_wake(p);
}

void bm_await_suspend(bm_promise *p) {
    bm_task *t = bm_cur_task;
    if (!t) bm_trap("internal error: await outside a task", NULL);
    p->handled = true;
    if (p->state != BM_PENDING) {
        bm_mq_push(bm_resume_job, t, NULL);
        return;
    }
    if (!p->waiter && !p->nmore) {
        p->waiter = t;
        return;
    }
    bm_promise_react(p, bm_resume_job, t, NULL);
}

/* One descriptor for every promise type: the value's type travels with the promise. */
static void bm_promise_ty_retain(void *p) { bm_promise_retain(*(bm_promise **)p); }
static void bm_promise_ty_release(void *p) { bm_promise_release(*(bm_promise **)p); }
static bool bm_promise_ty_eq(const void *a, const void *b) { return *(bm_promise *const *)a == *(bm_promise *const *)b; }
static uint64_t bm_promise_ty_hash(const void *p) { return bm_mix64((uint64_t)(uintptr_t)*(bm_promise *const *)p); }
static void bm_promise_ty_str(bm_sb *sb, const void *p) { (void)p; bm_sb_push_cstr(sb, "[object Promise]"); }
static void bm_promise_ty_inspect(bm_sb *sb, const void *pp, int depth) {
    bm_promise *p = *(bm_promise *const *)pp;
    bm_sb_push_cstr(sb, "Promise { ");
    if (p->state == BM_PENDING) bm_sb_push_cstr(sb, "<pending>");
    else if (p->state == BM_REJECTED) bm_sb_push_cstr(sb, "<rejected>");
    else if (p->vt) bm_inspect_value(sb, p->vt, p->value, depth + 1);
    else bm_sb_push_cstr(sb, "undefined");
    bm_sb_push_cstr(sb, " }");
}
const bm_type bm_type_promise = {sizeof(bm_promise *), bm_promise_ty_retain, bm_promise_ty_release, bm_promise_ty_eq, bm_promise_ty_hash, bm_promise_ty_str, NULL};

void bm_inspect_value(bm_sb *sb, const bm_type *t, const void *p, int depth) {
    if (t->inspect) t->inspect(sb, p, depth);
    else if (t == &bm_type_f64) bm_f64_inspect(sb, p, depth);
    else if (t == &bm_type_f32) bm_f32_inspect(sb, p, depth);
    else if (t == &bm_type_str) bm_inspect_str(sb, *(const bm_str *)p, depth);
    else if (t == &bm_type_promise) bm_promise_ty_inspect(sb, p, depth);
    else t->to_str(sb, p); /* integers, booleans, undefined: as String(x) */
}

/* ---------------------------------------------------------------- Promise.all, Promise.race */

typedef struct {
    bm_promise *result;
    bm_promise **elems;       /* retained until every reaction has run */
    int64_t n, remaining, pending_jobs;
    const bm_type *et, *arr_t;
    unsigned char *values;    /* all: the values so far (n * et->size), with `filled` flags */
    bool *filled;
} bm_combine;

static void bm_combine_done(bm_combine *s) {
    if (--s->pending_jobs) return;
    size_t size = s->et ? s->et->size : 0;
    for (int64_t i = 0; i < s->n; i++) {
        if (s->filled && s->filled[i] && s->et->release) s->et->release(s->values + (size_t)i * size);
        bm_promise_release(s->elems[i]);
    }
    bm_promise_release(s->result);
    bm_free(s->values);
    bm_free(s->filled);
    bm_free(s->elems);
    bm_free(s);
}

static bm_combine *bm_combine_new(bm_arr ps, const bm_type *et, const bm_type *rt, const bm_type *arr_t) {
    bm_combine *s = bm_alloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->n = s->remaining = s->pending_jobs = bm_arr_len(ps);
    s->et = et;
    s->arr_t = arr_t;
    s->result = bm_promise_new(rt);
    bm_promise_retain(s->result);   /* the state's reference */
    s->elems = bm_alloc((size_t)(s->n ? s->n : 1) * sizeof *s->elems);
    for (int64_t i = 0; i < s->n; i++) {
        s->elems[i] = ((bm_promise **)bm_arr_data(ps))[i];
        bm_promise_retain(s->elems[i]);
    }
    return s;
}

static void bm_all_step(void *a, void *b) {
    bm_combine *s = a;
    int64_t i = (int64_t)(intptr_t)b;
    bm_promise *p = s->elems[i];
    if (s->result->state == BM_PENDING) {
        if (p->state == BM_REJECTED) {
            if (bm_err_retain) bm_err_retain(p->err);
            bm_promise_reject(s->result, p->err);
        } else {
            size_t size = s->et->size;
            if (size) {
                memcpy(s->values + (size_t)i * size, p->value, size);
                if (s->et->retain) s->et->retain(s->values + (size_t)i * size);
            }
            s->filled[i] = true;
            if (--s->remaining == 0) {
                /* every value is in: move them into the result array */
                bm_arr arr = bm_arr_with_capacity(s->et, s->n);
                void *dst = bm_arr_reserve_tail(&arr, s->et, s->n);
                if (size) memcpy(dst, s->values, (size_t)s->n * size);
                arr.len = s->n;
                memset(s->filled, 0, (size_t)s->n * sizeof *s->filled);
                bm_promise_resolve_move(s->result, &arr);
            }
        }
    }
    bm_combine_done(s);
}

bm_promise *bm_promise_all(bm_arr ps, const bm_type *et, const bm_type *arr_t) {
    bm_combine *s = bm_combine_new(ps, et, arr_t, arr_t);
    bm_promise *result = s->result;
    if (s->n == 0) {
        /* nothing to wait for: fulfilled at once, with [] */
        bm_arr empty = BM_EMPTY_ARR;
        bm_promise_resolve_move(result, &empty);
        s->pending_jobs = 1;
        bm_combine_done(s);
        return result;
    }
    s->values = bm_alloc((size_t)s->n * (et->size ? et->size : 1));
    s->filled = bm_alloc((size_t)s->n * sizeof *s->filled);
    memset(s->filled, 0, (size_t)s->n * sizeof *s->filled);
    for (int64_t i = 0; i < s->n; i++) bm_promise_react(s->elems[i], bm_all_step, s, (void *)(intptr_t)i);
    return result;
}

static void bm_race_step(void *a, void *b) {
    bm_combine *s = a;
    bm_promise *p = s->elems[(int64_t)(intptr_t)b];
    if (s->result->state == BM_PENDING) {
        if (p->state == BM_REJECTED) {
            if (bm_err_retain) bm_err_retain(p->err);
            bm_promise_reject(s->result, p->err);
        } else {
            bm_promise_resolve(s->result, p->value);
        }
    }
    bm_combine_done(s);
}

bm_promise *bm_promise_race(bm_arr ps, const bm_type *et) {
    bm_combine *s = bm_combine_new(ps, et, et, NULL);
    bm_promise *result = s->result;
    if (s->n == 0) {   /* never settles, as in JavaScript */
        s->pending_jobs = 1;
        bm_combine_done(s);
        return result;
    }
    for (int64_t i = 0; i < s->n; i++) bm_promise_react(s->elems[i], bm_race_step, s, (void *)(intptr_t)i);
    return result;
}

typedef struct { bm_env h; bm_promise *p; } bm_resolver_env;

static void bm_resolver_drop(bm_env *e) { bm_promise_release(((bm_resolver_env *)e)->p); }

bm_env *bm_promise_resolver(bm_promise *p) {
    bm_resolver_env *e = bm_alloc(sizeof *e);
    e->h.rc = 1;
    e->h.drop = bm_resolver_drop;
    e->p = p;
    bm_promise_retain(p);
    return &e->h;
}

/* ---------------------------------------------------------------- timers (a min-heap by due time, then creation) */

/* As libuv (so as Node): the loop's clock is in whole milliseconds, read when the loop wakes up;
 * a timer is due at that time plus its delay, and due timers fire in the order they were set. */
typedef struct {
    uint64_t when;    /* due, in ms of the loop's clock */
    uint64_t seq;
    uint64_t every;   /* setInterval: period in ms (0: once) */
    bm_int id;
    bm_fn cb;         /* fn NULL: cleared */
    bool weak;        /* unref'd (AbortSignal.timeout): doesn't keep the program running */
} bm_timer;

static bm_timer *bm_timers;
static size_t bm_ntimers, bm_timers_cap, bm_live_timers;
static uint64_t bm_timer_seq;
static bm_int bm_timer_ids;

static uint64_t bm_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t bm_loop_ms;   /* the loop's clock (0: not read yet) */

static uint64_t bm_loop_update(void) { return bm_loop_ms = bm_mono_ns() / 1000000u; }
static uint64_t bm_loop_now(void) { return bm_loop_ms ? bm_loop_ms : bm_loop_update(); }

/* Sleeps until the loop's clock reads `ms`, precisely: a plain nanosleep wakes ~2 ms late on
 * macOS; a kqueue timer marked critical, ~0.1 ms. */
static void bm_sleep_until(uint64_t ms) {
    uint64_t deadline = ms * 1000000u;
    uint64_t now = bm_mono_ns();
    if (deadline <= now) return;
#if defined(__APPLE__) || defined(__FreeBSD__)
    static int kq = -1;
    if (kq < 0) kq = kqueue();
    if (kq >= 0) {
        struct kevent ch, ev;
        EV_SET(&ch, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, (int64_t)(deadline - now), 0);
        while (kevent(kq, &ch, 1, &ev, 1, NULL) < 0 && errno == EINTR) {}
        return;
    }
    struct timespec ts = {(time_t)((deadline - now) / 1000000000u), (long)((deadline - now) % 1000000000u)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
#else
    struct timespec ts = {(time_t)(deadline / 1000000000u), (long)(deadline % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
#endif
}

static bool bm_timer_before(const bm_timer *a, const bm_timer *b) { return a->when < b->when || (a->when == b->when && a->seq < b->seq); }

static void bm_timer_push(bm_timer t) {
    if (bm_ntimers == bm_timers_cap) {
        bm_timers_cap = bm_timers_cap ? bm_timers_cap * 2 : 16;
        bm_timers = bm_realloc(bm_timers, bm_timers_cap * sizeof *bm_timers);
    }
    size_t i = bm_ntimers++;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!bm_timer_before(&t, &bm_timers[parent])) break;
        bm_timers[i] = bm_timers[parent];
        i = parent;
    }
    bm_timers[i] = t;
}

static bm_timer bm_timer_pop(void) {
    bm_timer top = bm_timers[0];
    bm_timer last = bm_timers[--bm_ntimers];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        const bm_timer *best = &last;
        if (l < bm_ntimers && bm_timer_before(&bm_timers[l], best)) { m = l; best = &bm_timers[l]; }
        if (r < bm_ntimers && bm_timer_before(&bm_timers[r], best)) m = r;
        if (m == i) break;
        bm_timers[i] = bm_timers[m];
        i = m;
    }
    if (bm_ntimers) bm_timers[i] = last;
    return top;
}

bm_int bm_set_timer(bm_fn cb, double ms, bool repeat) {
    /* As Node: delays below 1 ms (or not a number, or too large) are 1 ms; fractions are dropped. */
    if (!(ms >= 1 && ms <= 2147483647.0)) ms = 1;
    uint64_t delay = (uint64_t)ms;
    bm_env_retain(cb.env);
    bm_timer t = {bm_loop_now() + delay, ++bm_timer_seq, repeat ? delay : 0, ++bm_timer_ids, cb, false};
    bm_timer_push(t);
    bm_live_timers++;
    return t.id;
}

void bm_clear_timer(bm_int id) {
    for (size_t i = 0; i < bm_ntimers; i++) {
        if (bm_timers[i].id == id && bm_timers[i].cb.fn) {
            bm_env_release(bm_timers[i].cb.env);
            bm_timers[i].cb.fn = NULL;
            if (!bm_timers[i].weak) bm_live_timers--;
            return;
        }
    }
}

/* Node's timer.unref(): the timer still fires while other work keeps the program running,
 * but doesn't keep it running by itself. */
void bm_native_timerUnref(bm_int id) {
    for (size_t i = 0; i < bm_ntimers; i++) {
        if (bm_timers[i].id == id && bm_timers[i].cb.fn && !bm_timers[i].weak) {
            bm_timers[i].weak = true;
            bm_live_timers--;
            return;
        }
    }
}

/* Fires every timer that is due, each followed by the microtasks it queued (as Node). */
static void bm_fire_timers(void) {
    uint64_t now = bm_loop_update();
    while (bm_ntimers && bm_timers[0].when <= now) {
        bm_timer t = bm_timer_pop();
        if (!t.cb.fn) continue;
        void (*fn)(bm_env *) = (void (*)(bm_env *))t.cb.fn;
        if (t.every) {
            bm_env_retain(t.cb.env);   /* the call's own reference: clearInterval may run inside it */
            t.when = now + t.every;
            t.seq = ++bm_timer_seq;
            bm_timer_push(t);
            fn(t.cb.env);
            bm_env_release(t.cb.env);
        } else {
            if (!t.weak) bm_live_timers--;
            fn(t.cb.env);
            bm_env_release(t.cb.env);
        }
        bm_run_microtasks();
    }
}

static bool bm_http_busy(void);

/* Node.js's check phase (setImmediate), for npm code (runtime/node.c): while pending, the loop
 * runs bm_loop_check once per turn, after timers and I/O, and doesn't block; a ref'd one keeps the
 * program running. */
void (*bm_loop_check)(void);
bool bm_loop_check_pending, bm_loop_check_ref;

static void bm_loop_run_check(void) {
    if (!bm_loop_check_pending || !bm_loop_check) return;
    bm_loop_check_pending = false;
    bm_loop_check();
}

void bm_async_run(void) {
    for (;;) {
        bm_run_microtasks();
        /* while servers run, their loop runs the timers too */
        if (bm_http_busy()) {
            bm_http_run();
            continue;
        }
        while (bm_ntimers && !bm_timers[0].cb.fn) (void)bm_timer_pop();
        bool check = bm_loop_check_pending;
        if (!bm_live_timers && !(check && bm_loop_check_ref)) return;
        if (bm_ntimers && !check && bm_timers[0].when > bm_loop_update()) bm_sleep_until(bm_timers[0].when);
        if (bm_ntimers) bm_fire_timers();
        bm_run_microtasks();
        bm_loop_run_check();
    }
}

/* ================================================================== HTTP server */

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <signal.h>
#include <stdatomic.h>
#include <strings.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define BM_KQUEUE 1
#else
#include <sys/epoll.h>
#endif

typedef struct bm_http_conn {
    int fd;
    struct bm_http_server *srv;
    struct bm_http_conn *prev, *next;
    char *in; size_t in_len, in_cap;
    char *out; size_t out_len, out_cap, out_off;
    bool close_after, want_write, want_read, stalled, continued;
    /* in/out currently point at the loop's shared buffers (see bm_http_attach) */
    bool in_shared, out_shared;
    bool eof;             /* the client closed its side */
    bool dirty;           /* on bm_http_dirty: a late response is ready to write */
    struct bm_http_conn *dirty_next;
    /* Requests answered later (async handlers), in request order; see bm_http_req. */
    struct bm_http_req *pend_head, *pend_tail;
    int npending;
} bm_http_conn;

/* A request whose response isn't written yet: its handler returned a promise (it has an id for
 * httpRespondTo), or it was answered while an earlier request on the same connection is still
 * waiting (responses go out in request order). */
typedef struct bm_http_req {
    struct bm_http_req *next;
    bm_http_conn *c;      /* NULL once the connection has closed */
    bm_int id;            /* 0: answered already, waiting for the ones ahead of it */
    bool keep, head, ready;
    bm_sb out;            /* the response, once ready */
} bm_http_req;

/* Pipelined requests handled ahead of a slow one, per connection, before reading pauses. */
#define BM_HTTP_MAX_PENDING 64

/* One read buffer and one write buffer per event loop: a connection borrows them while it is
 * handled, and keeps heap buffers only for leftovers (a partial request, or output the socket
 * didn't take). An idle keep-alive connection holds no buffers. */
#define BM_HTTP_RBUF (64 * 1024)
static char *bm_http_rbuf;
static char *bm_http_wbuf;
static size_t bm_http_wbuf_cap;

typedef struct bm_http_server {
    int fd;              /* -1 once stopped */
    bm_int port;
    bm_fn handler;
} bm_http_server;

#define BM_HTTP_MAX_SERVERS 64
static bm_http_server bm_http_servers[BM_HTTP_MAX_SERVERS];
static int bm_http_nservers, bm_http_active;
static bm_int bm_http_want_workers = 1;
static int bm_http_q = -1;               /* the event loop's kqueue/epoll (created on first use) */
static bm_http_conn *bm_http_conns;      /* open connections (for stop) */
static bm_http_conn *bm_http_cur;       /* the connection whose request is being handled */
static bool bm_http_keep;               /* the current request allows keep-alive */
static bool bm_http_head;               /* the current request is HEAD: send headers only */
static char bm_http_date[64];           /* "date: ...\r\n", refreshed once a second */
static time_t bm_http_date_at;
/* With several workers, each process accepts only while it holds no more connections than
 * the least-loaded worker (+1), so persistent connections spread evenly; counts live in a
 * shared mapping made before fork. */
static _Atomic int64_t *bm_http_loads;
static bm_int bm_http_nworkers = 1, bm_http_worker;
static pid_t bm_http_parent;   /* the supervisor, in worker processes */

static bool bm_http_may_accept(void) {
    if (!bm_http_loads) return true;
    int64_t mine = atomic_load_explicit(&bm_http_loads[bm_http_worker], memory_order_relaxed);
    for (bm_int i = 0; i < bm_http_nworkers; i++)
        if (atomic_load_explicit(&bm_http_loads[i], memory_order_relaxed) + 1 < mine) return false;
    return true;
}

static void bm_http_load_add(int64_t d) {
    if (bm_http_loads) atomic_fetch_add_explicit(&bm_http_loads[bm_http_worker], d, memory_order_relaxed);
}

static void bm_http_out(bm_http_conn *c, const char *s, size_t n) {
    if (c->out_cap - c->out_len < n) {
        size_t cap = c->out_cap ? c->out_cap * 2 : 4096;
        while (cap - c->out_len < n) cap *= 2;
        c->out = bm_realloc(c->out, cap);
        c->out_cap = cap;
        if (c->out_shared) { bm_http_wbuf = c->out; bm_http_wbuf_cap = cap; }
    }
    memcpy(c->out + c->out_len, s, n);
    c->out_len += n;
}

static const char *bm_http_reason(bm_int s) {
    switch (s) {
    case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted"; case 204: return "No Content";
    case 301: return "Moved Permanently"; case 302: return "Found"; case 304: return "Not Modified"; case 307: return "Temporary Redirect"; case 308: return "Permanent Redirect";
    case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
    case 409: return "Conflict"; case 413: return "Payload Too Large"; case 415: return "Unsupported Media Type"; case 422: return "Unprocessable Entity"; case 429: return "Too Many Requests";
    case 500: return "Internal Server Error"; case 501: return "Not Implemented"; case 502: return "Bad Gateway"; case 503: return "Service Unavailable";
    default: return "";
    }
}

static void bm_http_refresh_date(void) {
    time_t now = time(NULL);
    if (now == bm_http_date_at) return;
    bm_http_date_at = now;
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(bm_http_date, sizeof bm_http_date, "date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm);
}

static bool bm_has_header(bm_str block, const char *name) {
    size_t n = strlen(name);
    const char *s = block.p->data, *end = s + block.p->len;
    while (s < end) {
        if ((size_t)(end - s) > n && strncasecmp(s, name, n) == 0 && s[n] == ':') return true;
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        if (!nl) break;
        s = nl + 1;
    }
    return false;
}

/* Output goes to the connection (c) or to a response kept for later (sb). */
static inline void bm_http_put(bm_http_conn *c, bm_sb *sb, const char *s, size_t n) {
    if (c) bm_http_out(c, s, n);
    else bm_sb_push(sb, s, n);
}

static void bm_http_response(bm_http_conn *c, bm_sb *sb, bool keep, bool head, bm_int status, bm_str headers, bm_str body, bool typed) {
    char line[160], *p = line;
    memcpy(p, "HTTP/1.1 ", 9);
    p += 9;
    if (status < 100 || status > 999) status = 500;
    *p++ = (char)('0' + status / 100);
    *p++ = (char)('0' + status / 10 % 10);
    *p++ = (char)('0' + status % 10);
    *p++ = ' ';
    const char *reason = bm_http_reason(status);
    size_t rl = strlen(reason);
    memcpy(p, reason, rl);
    p += rl;
    *p++ = '\r';
    *p++ = '\n';
    /* 1xx, 204 and 304 responses carry no body (RFC 9110 §6.4.1) */
    bool bodiless = status < 200 || status == 204 || status == 304;
    if (!bodiless) {
        memcpy(p, "content-length: ", 16);
        p += 16;
        char digits[24];
        int nd = 0;
        uint64_t len = (uint64_t)body.p->len;
        do { digits[nd++] = (char)('0' + len % 10); len /= 10; } while (len);
        while (nd) *p++ = digits[--nd];
        *p++ = '\r';
        *p++ = '\n';
    }
    bm_http_put(c, sb, line, (size_t)(p - line));
    if (!bodiless && typed && !bm_has_header(headers, "content-type")) {
        static const char ct[] = "content-type: text/plain;charset=utf-8\r\n";
        bm_http_put(c, sb, ct, sizeof ct - 1);
    }
    bm_http_put(c, sb, bm_http_date, strlen(bm_http_date));
    if (!keep) {
        static const char cl[] = "connection: close\r\n";
        bm_http_put(c, sb, cl, sizeof cl - 1);
    }
    bm_http_put(c, sb, headers.p->data, (size_t)headers.p->len);
    bm_http_put(c, sb, "\r\n", 2);
    if (!bodiless && !head) bm_http_put(c, sb, body.p->data, (size_t)body.p->len);
}

static bm_http_conn *bm_http_dirty;   /* connections with a late response to write */

static void bm_http_mark_dirty(bm_http_conn *c) {
    if (c->dirty) return;
    c->dirty = true;
    c->dirty_next = bm_http_dirty;
    bm_http_dirty = c;
}

static void bm_http_enqueue(bm_http_conn *c, bm_http_req *r) {
    r->next = NULL;
    if (c->pend_tail) c->pend_tail->next = r; else c->pend_head = r;
    c->pend_tail = r;
    c->npending++;
}

/* Writes the ready responses at the front of c's queue, in order. */
static void bm_http_drain(bm_http_conn *c) {
    while (c->pend_head && c->pend_head->ready) {
        bm_http_req *r = c->pend_head;
        if (r->out.len) bm_http_out(c, r->out.data, r->out.len);
        if (!r->keep) c->close_after = true;
        c->pend_head = r->next;
        if (!c->pend_head) c->pend_tail = NULL;
        c->npending--;
        bm_sb_free(&r->out);
        bm_async_free(r, sizeof *r);
    }
}

void bm_native_httpRespond(bm_int status, bm_str headers, bm_str body, bool typed) {
    bm_http_conn *c = bm_http_cur;
    if (!c) return;
    bm_http_cur = NULL; /* one response per request */
    if (!c->npending) {
        bm_http_response(c, NULL, bm_http_keep, bm_http_head, status, headers, body, typed);
        return;
    }
    /* an earlier request on this connection is still waiting: this response waits behind it */
    bm_http_req *r = bm_async_alloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->c = c;
    r->keep = bm_http_keep;
    r->head = bm_http_head;
    r->ready = true;
    bm_http_response(NULL, &r->out, r->keep, r->head, status, headers, body, typed);
    bm_http_enqueue(c, r);
}

/* Requests waiting for httpRespondTo, by id (slot 0 unused; free slots are reused). */
static bm_http_req **bm_http_deferred;
static bm_int bm_http_ndeferred, bm_http_deferred_cap, bm_http_deferred_free;

bm_int bm_native_httpDefer(void) {
    bm_http_conn *c = bm_http_cur;
    if (!c) return 0;
    bm_http_cur = NULL;
    bm_http_req *r = bm_async_alloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->c = c;
    r->keep = bm_http_keep;
    r->head = bm_http_head;
    bm_int id;
    if (bm_http_deferred_free) {
        id = bm_http_deferred_free;
        bm_http_deferred_free = (bm_int)(intptr_t)bm_http_deferred[id];
    } else {
        if (bm_http_ndeferred + 1 >= bm_http_deferred_cap) {
            bm_http_deferred_cap = bm_http_deferred_cap ? bm_http_deferred_cap * 2 : 64;
            bm_http_deferred = bm_realloc(bm_http_deferred, (size_t)bm_http_deferred_cap * sizeof *bm_http_deferred);
        }
        id = ++bm_http_ndeferred;
    }
    bm_http_deferred[id] = r;
    r->id = id;
    bm_http_enqueue(c, r);
    return id;
}

void bm_native_httpRespondTo(bm_int id, bm_int status, bm_str headers, bm_str body, bool typed) {
    if (id <= 0 || id > bm_http_ndeferred) return;
    bm_http_req *r = bm_http_deferred[id];
    if (!r || (uintptr_t)r <= (uintptr_t)bm_http_ndeferred) return;   /* not waiting (a free slot) */
    bm_http_deferred[id] = (bm_http_req *)(intptr_t)bm_http_deferred_free;
    bm_http_deferred_free = id;
    r->id = 0;
    if (!r->c) { bm_async_free(r, sizeof *r); return; }   /* the client went away */
    bm_http_refresh_date();
    bm_http_conn *c = r->c;
    /* next in line: straight into the connection's output; otherwise it waits its turn */
    if (c->pend_head == r) bm_http_response(c, NULL, r->keep, r->head, status, headers, body, typed);
    else bm_http_response(NULL, &r->out, r->keep, r->head, status, headers, body, typed);
    r->ready = true;
    bm_http_drain(c);   /* writes (in order) and frees what's ready, r included */
    bm_http_mark_dirty(c);
}

bm_int bm_native_headerIndex(bm_str block, bm_str name) {
    size_t n = (size_t)name.p->len;
    const char *base = block.p->data, *s = base, *end = s + block.p->len;
    while (s < end) {
        if ((size_t)(end - s) > n && strncasecmp(s, name.p->data, n) == 0 && s[n] == ':') {
            const char *v = s + n + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            return (bm_int)(v - base);
        }
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        if (!nl) break;
        s = nl + 1;
    }
    return -1;
}

bm_str bm_native_headerValue(bm_str block, bm_int at) {
    const char *base = block.p->data, *s = base + at, *end = base + block.p->len;
    const char *e = s;
    while (e < end && *e != '\r' && *e != '\n') e++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    return bm_str_from(s, (size_t)(e - s));
}

bm_str bm_native_headerRemove(bm_str block, bm_str name) {
    size_t n = (size_t)name.p->len;
    const char *s = block.p->data, *end = s + block.p->len;
    bm_sb sb = {0};
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *next = nl ? nl + 1 : end;
        if (!((size_t)(end - s) > n && strncasecmp(s, name.p->data, n) == 0 && s[n] == ':')) bm_sb_push(&sb, s, (size_t)(next - s));
        s = next;
    }
    return bm_str_from_sb(&sb);
}

/* block + "name: value\r\n"; drops bytes that would let a name or value end the line early
 * (CR, LF, NUL; and ':' or whitespace in the name), so user headers can't split a response. */
bm_str bm_native_headerAppend(bm_str block, bm_str name, bm_str value) {
    bm_sb sb = {0};
    bm_sb_push(&sb, block.p->data, (size_t)block.p->len);
    for (int64_t i = 0; i < name.p->len; i++) {
        char ch = name.p->data[i];
        if (ch != '\r' && ch != '\n' && ch != '\0' && ch != ':' && ch != ' ' && ch != '\t') bm_sb_push_char(&sb, ch);
    }
    bm_sb_push(&sb, ": ", 2);
    for (int64_t i = 0; i < value.p->len; i++) {
        char ch = value.p->data[i];
        if (ch != '\r' && ch != '\n' && ch != '\0') bm_sb_push_char(&sb, ch);
    }
    bm_sb_push(&sb, "\r\n", 2);
    return bm_str_from_sb(&sb);
}

BM_STR_LIT(bm_lit_host, "host");

/* A request's full URL: "http://" + Host header (or localhost) + target. */
bm_str bm_native_requestUrl(bm_str headers, bm_str target) {
    bm_int at = bm_native_headerIndex(headers, (bm_str){(bm_strbuf *)&bm_lit_host});
    const char *host = "localhost";
    size_t hl = 9;
    if (at >= 0) {
        const char *base = headers.p->data, *e = base + at, *end = base + headers.p->len;
        while (e < end && *e != '\r' && *e != '\n') e++;
        while (e > base + at && (e[-1] == ' ' || e[-1] == '\t')) e--;
        host = base + at;
        hl = (size_t)(e - host);
    }
    bm_sb sb = {0};
    bm_sb_grow(&sb, 7 + hl + (size_t)target.p->len + 1);
    bm_sb_push(&sb, "http://", 7);
    bm_sb_push(&sb, host, hl);
    bm_sb_push(&sb, target.p->data, (size_t)target.p->len);
    return bm_str_from_sb(&sb);
}

BM_STR_LIT(bm_lit_get, "GET");
BM_STR_LIT(bm_lit_post, "POST");

#define BM_HTTP_MAX_HEAD (64 * 1024)
#define BM_HTTP_MAX_BODY (64 * 1024 * 1024)
#define BM_HTTP_OUT_HIGH (1024 * 1024)   /* stop handling pipelined requests until flushed */

/* A canned error response; the connection closes after it is written. */
static void bm_http_fail(bm_http_conn *c, const char *status) {
    char buf[160];
    int n = snprintf(buf, sizeof buf, "HTTP/1.1 %s\r\ncontent-length: 0\r\nconnection: close\r\n\r\n", status);
    bm_http_out(c, buf, (size_t)n);
    c->close_after = true;
}

static bool bm_http_token_eq(const char *v, const char *end, const char *word) {
    size_t n = strlen(word);
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    while (end > v && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    return (size_t)(end - v) == n && strncasecmp(v, word, n) == 0;
}

/* Decodes a chunked body starting at `p`; returns the bytes consumed (through the trailers),
 * 0 if incomplete, or -1 if malformed / too large. */
static long long bm_http_dechunk(const char *p, const char *end, bm_sb *out) {
    const char *s = p;
    for (;;) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        if (!nl) return 0;
        uint64_t size = 0;
        const char *d = s;
        int digits = 0;
        for (; d < nl; d++, digits++) {
            int v = *d >= '0' && *d <= '9' ? *d - '0' : (*d | 32) >= 'a' && (*d | 32) <= 'f' ? (*d | 32) - 'a' + 10 : -1;
            if (v < 0) break;
            size = size * 16 + (uint64_t)v;
            if (size > BM_HTTP_MAX_BODY) return -1;
        }
        if (!digits || (*d != '\r' && *d != ';')) return -1;
        s = nl + 1;
        if (size == 0) {
            /* trailers: lines until an empty one */
            for (;;) {
                const char *t = memchr(s, '\n', (size_t)(end - s));
                if (!t) return 0;
                bool empty = t == s || (t == s + 1 && *s == '\r');
                s = t + 1;
                if (empty) return (long long)(s - p);
            }
        }
        if ((uint64_t)(end - s) < size + 2) return 0;
        if (out->len + size > BM_HTTP_MAX_BODY) return -1;
        bm_sb_push(out, s, (size_t)size);
        s += size;
        if (s[0] != '\r' || s[1] != '\n') return -1;
        s += 2;
    }
}

/* Handles every complete request in c->in (stopping early when the output backs up, which
 * sets c->stalled); returns false if the connection must close at once. */
static bool bm_http_process(bm_http_conn *c) {
    bm_fn h = c->srv->handler;
    size_t pos = 0;
    c->stalled = false;
    while (pos < c->in_len && !c->close_after && c->npending < BM_HTTP_MAX_PENDING) {
        if (c->out_len >= BM_HTTP_OUT_HIGH) { c->stalled = true; break; }
        char *start = c->in + pos;
        size_t avail = c->in_len - pos;
        /* tolerate blank lines between requests (RFC 9112 §2.2) */
        if (start[0] == '\r' || start[0] == '\n') { pos++; continue; }
        char *hdr_end = NULL;
        for (char *q = start; (q = memchr(q, '\n', avail - (size_t)(q - start))) != NULL; q++) {
            if (q + 2 < start + avail && q[1] == '\r' && q[2] == '\n') { hdr_end = q + 3; break; }
            if (q + 1 < start + avail && q[1] == '\n') { hdr_end = q + 2; break; }
        }
        if (!hdr_end) {
            if (avail > BM_HTTP_MAX_HEAD) bm_http_fail(c, "431 Request Header Fields Too Large");
            break;
        }
        if ((size_t)(hdr_end - start) > BM_HTTP_MAX_HEAD) { bm_http_fail(c, "431 Request Header Fields Too Large"); break; }
        /* Request line: METHOD SP TARGET SP HTTP/1.x CRLF */
        char *line_end = memchr(start, '\n', (size_t)(hdr_end - start));
        char *eol = line_end > start && line_end[-1] == '\r' ? line_end - 1 : line_end;
        char *sp1 = memchr(start, ' ', (size_t)(eol - start));
        char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
        if (!sp1 || sp1 == start || !sp2 || sp2 == sp1 + 1 || eol - sp2 != 9 || memcmp(sp2 + 1, "HTTP/1.", 7) != 0 || (sp2[8] != '0' && sp2[8] != '1')) {
            bm_http_fail(c, "400 Bad Request");
            break;
        }
        bool http10 = sp2[8] == '0';
        char *headers = line_end + 1;
        char *headers_end = hdr_end - (hdr_end[-2] == '\r' ? 2 : 1);
        long long clen = -1;
        bool keep = !http10, chunked = false, expect = false, bad = false;
        for (char *s = headers; s < headers_end;) {
            char *nl = memchr(s, '\n', (size_t)(hdr_end - s));
            size_t len = (size_t)(nl - s);
            if (len > 15 && strncasecmp(s, "content-length:", 15) == 0) {
                const char *v = s + 15, *e = nl;
                while (v < e && (*v == ' ' || *v == '\t')) v++;
                while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
                long long n = 0;
                if (v == e) bad = true;
                for (; v < e; v++) {
                    if (*v < '0' || *v > '9') { bad = true; break; }
                    if (n <= BM_HTTP_MAX_BODY) n = n * 10 + (*v - '0'); /* saturates: over the cap → 413 */
                }
                if (clen >= 0 && clen != n) bad = true;
                clen = n;
            } else if (len > 18 && strncasecmp(s, "transfer-encoding:", 18) == 0) {
                if (bm_http_token_eq(s + 18, nl, "chunked")) chunked = true;
                else bad = true; /* other codings are not supported */
            } else if (len > 11 && strncasecmp(s, "connection:", 11) == 0) {
                if (bm_http_token_eq(s + 11, nl, "close")) keep = false;
                else if (bm_http_token_eq(s + 11, nl, "keep-alive")) keep = true;
            } else if (len > 7 && strncasecmp(s, "expect:", 7) == 0) {
                expect = bm_http_token_eq(s + 7, nl, "100-continue");
            }
            s = nl + 1;
        }
        if (bad || (chunked && clen >= 0)) { bm_http_fail(c, "400 Bad Request"); break; }
        if (clen > BM_HTTP_MAX_BODY) { bm_http_fail(c, "413 Payload Too Large"); break; }
        char *in_end = c->in + c->in_len;
        size_t consumed;
        bm_str body;
        if (chunked) {
            bm_sb sb = {0};
            long long n = bm_http_dechunk(hdr_end, in_end, &sb);
            if (n < 0) { bm_sb_free(&sb); bm_http_fail(c, "400 Bad Request"); break; }
            if (n == 0) {
                bm_sb_free(&sb);
                if (expect && !c->continued) { bm_http_out(c, "HTTP/1.1 100 Continue\r\n\r\n", 25); c->continued = true; }
                break;
            }
            consumed = (size_t)(hdr_end - start) + (size_t)n;
            body = bm_str_from_sb(&sb);
        } else {
            if (clen < 0) clen = 0;
            if ((size_t)(in_end - hdr_end) < (size_t)clen) {
                if (expect && !c->continued) { bm_http_out(c, "HTTP/1.1 100 Continue\r\n\r\n", 25); c->continued = true; }
                break; /* body incomplete */
            }
            consumed = (size_t)(hdr_end - start) + (size_t)clen;
            body = clen ? bm_str_from(hdr_end, (size_t)clen) : BM_EMPTY_STR;
        }
        c->continued = false;
        size_t mlen = (size_t)(sp1 - start);
        bm_str method = mlen == 3 && memcmp(start, "GET", 3) == 0 ? BM_LIT(bm_lit_get) : mlen == 4 && memcmp(start, "POST", 4) == 0 ? BM_LIT(bm_lit_post) : bm_str_from(start, mlen);
        bm_str target = bm_str_from(sp1 + 1, (size_t)(sp2 - sp1 - 1));
        bm_str hdrs = bm_str_from(headers, (size_t)(headers_end - headers));
        bm_http_cur = c;
        bm_http_keep = keep;
        bm_http_head = mlen == 4 && memcmp(start, "HEAD", 4) == 0;
        ((void (*)(void *, bm_str, bm_str, bm_str, bm_str))h.fn)(h.env, method, target, hdrs, body);
        if (bm_http_cur) bm_native_httpRespond(500, BM_EMPTY_STR, BM_EMPTY_STR, false); /* no response */
        bm_run_microtasks();   /* each request is a macrotask, as in JavaScript */
        bm_str_release(method);
        bm_str_release(target);
        bm_str_release(hdrs);
        bm_str_release(body);
        pos += consumed;
        if (!keep) c->close_after = true;
    }
    if (pos) {
        memmove(c->in, c->in + pos, c->in_len - pos);
        c->in_len -= pos;
    }
    return true;
}

static bool bm_http_flush(bm_http_conn *c) {
    while (c->out_off < c->out_len) {
        ssize_t w = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
        c->out_off += (size_t)w;
    }
    c->out_off = c->out_len = 0;
    return true;
}

static bm_http_conn bm_http_dead;   /* stands in for connections closed earlier in an event batch */

static void bm_http_close(bm_http_conn *c) {
    bm_http_load_add(-1);
    if (c->dirty) {
        for (bm_http_conn **pp = &bm_http_dirty; *pp; pp = &(*pp)->dirty_next)
            if (*pp == c) { *pp = c->dirty_next; break; }
    }
    /* waiting requests outlive the connection until they're answered (then dropped) */
    for (bm_http_req *r = c->pend_head, *next; r; r = next) {
        next = r->next;
        if (r->id) {
            r->c = NULL;
        } else {
            bm_sb_free(&r->out);
            bm_async_free(r, sizeof *r);
        }
    }
    if (c->prev) c->prev->next = c->next; else bm_http_conns = c->next;
    if (c->next) c->next->prev = c->prev;
    close(c->fd);
    if (!c->in_shared) bm_free(c->in);
    if (!c->out_shared) bm_free(c->out);
    bm_free(c);
}

/* Lends the shared buffers to c (when it has no leftovers of its own). */
static void bm_http_attach(bm_http_conn *c) {
    if (c->in_len == 0 && !c->in_shared) {
        bm_free(c->in);
        if (!bm_http_rbuf) bm_http_rbuf = bm_alloc(BM_HTTP_RBUF); /* only the pages reads touch are resident */
        c->in = bm_http_rbuf;
        c->in_cap = BM_HTTP_RBUF;
        c->in_shared = true;
    }
    if (c->out_len == 0 && !c->out_shared) {
        bm_free(c->out);
        if (!bm_http_wbuf) { bm_http_wbuf_cap = 64 * 1024; bm_http_wbuf = bm_alloc(bm_http_wbuf_cap); }
        c->out = bm_http_wbuf;
        c->out_cap = bm_http_wbuf_cap;
        c->out_off = 0;
        c->out_shared = true;
    }
}

/* Gives the shared buffers back, copying any leftovers to c's own (exact-size) buffers. */
static void bm_http_detach(bm_http_conn *c) {
    if (c->in_shared) {
        c->in_shared = false;
        if (c->in_len) {
            size_t cap = c->in_len < 4096 ? 4096 : c->in_len * 2;
            char *p = bm_alloc(cap);
            memcpy(p, c->in, c->in_len);
            c->in = p;
            c->in_cap = cap;
        } else {
            c->in = NULL;
            c->in_cap = 0;
        }
    }
    if (c->out_shared) {
        c->out_shared = false;
        size_t left = c->out_len - c->out_off;
        if (left) {
            char *p = bm_alloc(left);
            memcpy(p, c->out + c->out_off, left);
            c->out = p;
            c->out_cap = c->out_len = left;
        } else {
            c->out = NULL;
            c->out_cap = c->out_len = 0;
        }
        c->out_off = 0;
        /* a huge response grew the shared buffer: don't keep it */
        if (bm_http_wbuf_cap > (size_t)1 << 20) {
            bm_free(bm_http_wbuf);
            bm_http_wbuf = NULL;
            bm_http_wbuf_cap = 0;
        }
    }
    if (c->in && c->in_len == 0) { bm_free(c->in); c->in = NULL; c->in_cap = 0; }
    if (c->out && c->out_len == 0) { bm_free(c->out); c->out = NULL; c->out_cap = 0; c->out_off = 0; }
}

/* ---- servers: registered by listen (at any time), served by bm_http_run after the program */


/* Other I/O on the loop (the fetch client): its events' udata is a bm_io pointer with bit 0
 * set. bm_io_refs counts what keeps the loop running (requests in flight, DNS lookups); idle
 * pooled connections don't. After each batch of events the loop calls bm_io_after_batch (which
 * frees handles closed during the batch). Not static: nothing binds the loop to the client's
 * code, so programs that never fetch don't link it. */
typedef struct bm_io bm_io;
struct bm_io { void (*ready)(bm_io *h, bool readable, bool writable, bool broken); };
int bm_io_refs;
void (*bm_io_after_batch)(void);
void (*bm_io_after_fork)(void);

static int bm_loop_queue(void) {
    if (bm_http_q < 0) {
#ifdef BM_KQUEUE
        bm_http_q = kqueue();
#else
        bm_http_q = epoll_create1(EPOLL_CLOEXEC);
#endif
        if (bm_http_q < 0) bm_trap("can't create the event loop", NULL);
        fcntl(bm_http_q, F_SETFD, FD_CLOEXEC);
    }
    return bm_http_q;
}

static bool bm_http_is_server(void *p) {
    return (char *)p >= (char *)bm_http_servers && (char *)p < (char *)(bm_http_servers + BM_HTTP_MAX_SERVERS);
}

static void bm_http_watch_listener(bm_http_server *sv) {
#ifdef BM_KQUEUE
    struct kevent ev;
    EV_SET(&ev, sv->fd, EVFILT_READ, EV_ADD, 0, 0, sv);
    kevent(bm_http_q, &ev, 1, NULL, 0, NULL);
#else
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = sv };
    epoll_ctl(bm_http_q, EPOLL_CTL_ADD, sv->fd, &ev);
#endif
}

static void bm_http_accept(bm_http_server *sv) {
    while (sv->fd >= 0 && bm_http_may_accept()) {
        int fd = accept(sv->fd, NULL, NULL);
        if (fd < 0) break;
        bm_http_load_add(1);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        bm_http_conn *nc = bm_alloc(sizeof *nc);
        memset(nc, 0, sizeof *nc);
        nc->fd = fd;
        nc->srv = sv;
        nc->want_read = true;
        nc->next = bm_http_conns;
        if (bm_http_conns) bm_http_conns->prev = nc;
        bm_http_conns = nc;
#ifdef BM_KQUEUE
        struct kevent cev;
        EV_SET(&cev, fd, EVFILT_READ, EV_ADD, 0, 0, nc);
        kevent(bm_http_q, &cev, 1, NULL, 0, NULL);
#else
        struct epoll_event cev = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = nc };
        epoll_ctl(bm_http_q, EPOLL_CTL_ADD, fd, &cev);
#endif
    }
}

/* Reads (when readable), handles the complete requests, writes; closes the connection once it's
 * finished. Returns false if it was closed. */
static bool bm_http_service(bm_http_conn *c, bool readable, bool broken) {
    bool ok = !broken;
    bm_http_attach(c);
    if (readable && c->want_read) {
        for (;;) {
            if (c->in_cap - c->in_len < 4096) {
                size_t cap = c->in_cap ? c->in_cap * 2 : 8192;
                if (c->in_shared) { /* more than the shared buffer holds: move to its own */
                    char *p = bm_alloc(cap);
                    memcpy(p, c->in, c->in_len);
                    c->in = p;
                    c->in_shared = false;
                } else {
                    c->in = bm_realloc(c->in, cap);
                }
                c->in_cap = cap;
            }
            size_t room = c->in_cap - c->in_len;
            ssize_t r = read(c->fd, c->in + c->in_len, room);
            /* Level-triggered: a short read means the socket is drained, so skip the read that
             * would only return EAGAIN (one syscall per request saved). */
            if (r > 0) { c->in_len += (size_t)r; if ((size_t)r == room && c->in_len < BM_HTTP_MAX_BODY + BM_HTTP_MAX_HEAD) continue; break; }
            if (r == 0) { c->eof = true; break; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
    }
    /* handle, write, and handle again while flushing unblocks pipelined requests */
    while (ok) {
        if (c->in_len && !c->close_after && c->npending < BM_HTTP_MAX_PENDING) ok = bm_http_process(c);
        if (ok && c->out_len) ok = bm_http_flush(c);
        if (!(ok && c->stalled && c->out_len == 0)) break;
    }
    /* a stopped server finishes the requests in hand, then closes its connections */
    if (c->srv->fd < 0 && c->in_len == 0) c->close_after = true;
    if (ok && c->out_len == 0 && c->npending == 0 && (c->close_after || c->eof)) ok = false;
    if (!ok) {
        bm_http_detach(c);
        bm_http_close(c);
        return false;
    }
    bm_http_detach(c);
    /* interest: write while output is pending; read unless output is backed up, or too many
     * requests are waiting for their responses */
    bool want_write = c->out_len > 0;
    bool want_read = !(c->stalled || c->eof || (c->close_after && c->out_len) || c->npending >= BM_HTTP_MAX_PENDING);
#ifdef BM_KQUEUE
    struct kevent mods[2];
    int nm = 0;
    if (want_write != c->want_write) EV_SET(&mods[nm++], c->fd, EVFILT_WRITE, want_write ? EV_ADD : EV_DELETE, 0, 0, c);
    if (want_read != c->want_read) EV_SET(&mods[nm++], c->fd, EVFILT_READ, want_read ? EV_ENABLE : EV_DISABLE, 0, 0, c);
    if (nm) kevent(bm_http_q, mods, nm, NULL, 0, NULL);
#else
    if (want_write != c->want_write || want_read != c->want_read) {
        struct epoll_event wev = { .events = (want_read ? EPOLLIN | EPOLLRDHUP : 0) | (want_write ? EPOLLOUT : 0), .data.ptr = c };
        epoll_ctl(bm_http_q, EPOLL_CTL_MOD, c->fd, &wev);
    }
#endif
    c->want_write = want_write;
    c->want_read = want_read;
    return true;
}

static char bm_http_timer_tag;   /* the loop's timer event (the next setTimeout) */

/* Runs the event loop until every server has stopped and its connections have closed; timers
 * and microtasks run in it too. */
static void bm_http_loop(void) {
    bm_loop_queue();
#ifdef BM_KQUEUE
    struct kevent events[256];
#else
    struct epoll_event events[256];
#endif
    for (int i = 0; i < bm_http_nservers; i++)
        if (bm_http_servers[i].fd >= 0) bm_http_watch_listener(&bm_http_servers[i]);
#ifdef BM_KQUEUE
    if (bm_http_parent) { /* a worker exits with its supervisor (Linux uses PR_SET_PDEATHSIG) */
        struct kevent ev;
        EV_SET(&ev, bm_http_parent, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, &bm_http_dead);
        if (kevent(bm_http_q, &ev, 1, NULL, 0, NULL) != 0) _exit(0);
    }
#endif
    while (bm_http_active > 0 || bm_http_conns || bm_io_refs > 0 || (bm_loop_check_pending && bm_loop_check_ref)) {
        bm_run_microtasks();
        /* connections whose late responses are ready */
        while (bm_http_dirty) {
            bm_http_conn *c = bm_http_dirty;
            bm_http_dirty = c->dirty_next;
            c->dirty = false;
            bm_http_service(c, false, false);
            bm_run_microtasks();
        }
        if (!(bm_http_active > 0 || bm_http_conns || bm_io_refs > 0 || (bm_loop_check_pending && bm_loop_check_ref))) break;
        if (bm_out_len) bm_out_flush(); /* handler logs reach pipes and files promptly */
        while (bm_ntimers && !bm_timers[0].cb.fn) (void)bm_timer_pop();
        /* wait for I/O, or until the next timer is due */
        bool timed = bm_ntimers > 0; /* unref'd timers fire too while the loop runs */
        uint64_t now_ms = bm_loop_update();
        uint64_t due = timed ? bm_timers[0].when : 0;
#ifdef BM_KQUEUE
        struct kevent tch;
        int nch = 0;
        struct timespec zero = {0, 0};
        if (timed && due > now_ms) {
            /* a kqueue timer marked critical wakes within ~0.1 ms (a plain timeout: ~1 ms late) */
            uint64_t wait = due * 1000000u > bm_mono_ns() ? due * 1000000u - bm_mono_ns() : 0;
            EV_SET(&tch, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, (int64_t)wait, &bm_http_timer_tag);
            nch = 1;
        }
        /* (a pending check phase doesn't wait) */
        int n = kevent(bm_http_q, &tch, nch, events, 256, (timed && due <= now_ms) || bm_loop_check_pending ? &zero : NULL);
#else
        int timeout = -1;
        if (timed) timeout = due > now_ms ? (int)(due - now_ms) : 0;
        if (bm_loop_check_pending) timeout = 0; /* (a pending check phase doesn't wait) */
        int n = epoll_wait(bm_http_q, events, 256, timeout);
#endif
        if (n < 0) { if (errno == EINTR) continue; break; }
        bm_http_refresh_date();
        if (timed) bm_fire_timers();
        for (int i = 0; i < n; i++) {
#ifdef BM_KQUEUE
            void *tag = events[i].udata;
            bool readable = events[i].filter == EVFILT_READ, broken = (events[i].flags & EV_ERROR) != 0;
#else
            void *tag = events[i].data.ptr;
            bool readable = events[i].events & (EPOLLIN | EPOLLRDHUP), broken = events[i].events & (EPOLLHUP | EPOLLERR);
#endif
            if (tag == &bm_http_timer_tag) continue;
            if ((uintptr_t)tag & 1) {
                bm_io *h = (bm_io *)((uintptr_t)tag - 1);
#ifdef BM_KQUEUE
                h->ready(h, events[i].filter == EVFILT_READ, events[i].filter == EVFILT_WRITE, broken);
#else
                h->ready(h, (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0, (events[i].events & (EPOLLOUT | EPOLLERR)) != 0, false);
#endif
                continue;
            }
            if (bm_http_is_server(tag)) {
                bm_http_accept(tag);
                continue;
            }
            bm_http_conn *c = tag;
            if (c == &bm_http_dead) {
#ifdef BM_KQUEUE
                if (events[i].filter == EVFILT_PROC) _exit(0);
#endif
                continue;
            }
            if (!bm_http_service(c, readable, broken)) {
                for (int j = i + 1; j < n; j++) {
#ifdef BM_KQUEUE
                    if (events[j].udata == c) events[j].udata = &bm_http_dead;
#else
                    if (events[j].data.ptr == c) events[j].data.ptr = &bm_http_dead;
#endif
                }
            }
        }
        if (bm_io_after_batch) bm_io_after_batch();
        bm_run_microtasks();
        bm_loop_run_check();
    }
    /* the queue stays: pooled client connections are still registered with it */
}

/* ---- workers: the parent supervises; each child runs the event loop */

static volatile sig_atomic_t bm_http_stop_sig;

static pid_t *volatile bm_http_kids;
static bm_int bm_http_nkids;

/* May run on any thread (the program runs off the main thread), so it stops the workers
 * itself; their exit wakes the supervisor's waitpid. */
static void bm_http_on_stop(int sig) {
    bm_http_stop_sig = sig;
    pid_t *kids = bm_http_kids;
    if (kids)
        for (bm_int w = 0; w < bm_http_nkids; w++)
            if (kids[w] > 0) kill(kids[w], SIGTERM);
}

static pid_t bm_http_spawn(bm_int w) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    /* child */
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    bm_http_worker = w;
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    if (getppid() != bm_http_parent) _exit(0); /* the supervisor died before prctl */
    /* the parent's event queue (epoll's is shared after fork) and client state aren't ours */
    if (bm_http_q >= 0) { close(bm_http_q); bm_http_q = -1; }
    if (bm_io_after_fork) bm_io_after_fork();
    /* Give back the free malloc pages inherited from the program's setup. */
#if defined(__APPLE__)
    malloc_zone_pressure_relief(NULL, 0);
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
    bm_http_refresh_date();
    bm_http_loop();
    bm_out_flush();
    _exit(0);
}

/* Forks `workers` children and keeps them running: a worker that dies (a trap in a handler)
 * is replaced, after a second's pause if it died within a second of starting. SIGTERM and
 * SIGINT stop the workers and then the supervisor. */
static void bm_http_supervise(bm_int workers) {
    bm_http_parent = getpid();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = bm_http_on_stop; /* no SA_RESTART: waitpid returns EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    pid_t *kids = bm_alloc((size_t)workers * sizeof *kids);
    time_t *born = bm_alloc((size_t)workers * sizeof *born);
    for (bm_int w = 0; w < workers; w++) kids[w] = 0;
    bm_http_nkids = workers;
    bm_http_kids = kids;
    for (bm_int w = 0; w < workers; w++) {
        kids[w] = bm_http_spawn(w);
        born[w] = time(NULL);
    }
    bm_int alive = workers;
    while (!bm_http_stop_sig && alive > 0) {
        int st;
        pid_t pid = waitpid(-1, &st, 0);
        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (bm_int w = 0; w < workers; w++) {
            if (kids[w] != pid) continue;
            if (bm_http_stop_sig) break;
            /* a worker whose servers all stopped exits cleanly; one that crashed is replaced */
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                kids[w] = 0;
                alive--;
                break;
            }
            /* Killed by a stop signal (the process group was signalled, and this thread saw the
             * worker exit before the handler ran): that's a stop, not a crash. */
            if (WIFSIGNALED(st) && (WTERMSIG(st) == SIGTERM || WTERMSIG(st) == SIGINT || WTERMSIG(st) == SIGKILL)) {
                kids[w] = 0;
                bm_http_stop_sig = WTERMSIG(st) == SIGKILL ? SIGTERM : WTERMSIG(st);
                break;
            }
            if (time(NULL) - born[w] < 1) sleep(1); /* crash loop: don't spin */
            if (bm_http_loads) atomic_store(&bm_http_loads[w], 0);
            kids[w] = bm_http_spawn(w);
            born[w] = time(NULL);
        }
    }
    if (!bm_http_stop_sig) return; /* every worker finished */
    int sig = bm_http_stop_sig ? bm_http_stop_sig : SIGTERM;
    for (bm_int w = 0; w < workers; w++) if (kids[w] > 0) kill(kids[w], SIGTERM);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
    bm_out_flush();
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

static bool bm_http_busy(void) { return bm_http_active > 0 || bm_http_conns || bm_io_refs > 0; }

void bm_http_run(void) {
    if (!bm_http_busy()) return;
    bm_out_flush();
    bm_int workers = bm_http_want_workers;
    if (workers <= 1 || bm_http_active == 0) {
        bm_http_refresh_date();
        bm_http_loop();
        return;
    }
    void *m = mmap(NULL, (size_t)workers * sizeof(int64_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (m != MAP_FAILED) { bm_http_loads = m; bm_http_nworkers = workers; }
    bm_http_supervise(workers);
    /* every worker has finished: so have this process's servers */
    for (int i = 0; i < bm_http_nservers; i++)
        if (bm_http_servers[i].fd >= 0) { close(bm_http_servers[i].fd); bm_http_servers[i].fd = -1; }
    bm_http_active = 0;
}

bm_int bm_native_httpListen(bm_int port, bm_str host, bm_fn handler) {
    signal(SIGPIPE, SIG_IGN);
    if (bm_http_nservers == BM_HTTP_MAX_SERVERS) { bm_sb_push_cstr(&bm_native_err, "too many servers"); return -1; }
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { bm_native_fail(errno, "socket", host); return -1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    const char *h = host.p->len ? host.p->data : "0.0.0.0";
    if (strcmp(h, "localhost") == 0) h = "127.0.0.1";
    if (inet_pton(AF_INET, h, &addr.sin_addr) != 1) { bm_native_fail(EINVAL, "listen", host); close(lfd); return -1; }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        bm_sb_push_cstr(&bm_native_err, errno == EADDRINUSE ? "Failed to start server. Is port " : "Failed to start server on port ");
        bm_sb_push_int(&bm_native_err, port);
        bm_sb_push_cstr(&bm_native_err, errno == EADDRINUSE ? " in use?" : "");
        close(lfd);
        return -1;
    }
    if (listen(lfd, 4096) != 0) { bm_native_fail(errno, "listen", host); close(lfd); return -1; }
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK);
    socklen_t len = sizeof addr;
    getsockname(lfd, (struct sockaddr *)&addr, &len);
    int id = bm_http_nservers++;
    bm_env_retain(handler.env); /* kept for the life of the program */
    bm_http_servers[id] = (bm_http_server){ .fd = lfd, .port = ntohs(addr.sin_port), .handler = handler };
    bm_http_active++;
    if (bm_http_q >= 0) bm_http_watch_listener(&bm_http_servers[id]); /* started from a handler */
    return id;
}

bm_int bm_native_httpPort(bm_int id) { return id >= 0 && id < bm_http_nservers ? bm_http_servers[id].port : 0; }

/* Stops accepting; open connections finish the request in hand and close. */
void bm_native_httpStop(bm_int id, bool force) {
    if (id < 0 || id >= bm_http_nservers || bm_http_servers[id].fd < 0) return;
    close(bm_http_servers[id].fd);
    bm_http_servers[id].fd = -1;
    bm_http_active--;
    for (bm_http_conn *c = bm_http_conns, *next; c; c = next) {
        next = c->next;
        if (c->srv != &bm_http_servers[id]) continue;
        if (force || (c->in_len == 0 && c->out_len == 0 && c != bm_http_cur)) {
            if (c == bm_http_cur) { c->close_after = true; continue; }
            bm_http_close(c);
        } else {
            c->close_after = true;
        }
    }
}

void bm_native_httpWorkers(bm_int n) {
    if (n > bm_http_want_workers) bm_http_want_workers = n > 1024 ? 1024 : n;
}

/* ================================================================== URLs */

static bool bm_url_special(const char *scheme, size_t n) {
    return (n == 4 && memcmp(scheme, "http", 4) == 0) || (n == 5 && memcmp(scheme, "https", 5) == 0) || (n == 2 && memcmp(scheme, "ws", 2) == 0) ||
           (n == 3 && memcmp(scheme, "wss", 3) == 0) || (n == 3 && memcmp(scheme, "ftp", 3) == 0) || (n == 4 && memcmp(scheme, "file", 4) == 0);
}

static const char *bm_url_default_port(const char *scheme, size_t n) {
    if (n == 4 && memcmp(scheme, "http", 4) == 0) return "80";
    if (n == 5 && memcmp(scheme, "https", 5) == 0) return "443";
    if (n == 2 && memcmp(scheme, "ws", 2) == 0) return "80";
    if (n == 3 && memcmp(scheme, "wss", 3) == 0) return "443";
    if (n == 3 && memcmp(scheme, "ftp", 3) == 0) return "21";
    return NULL;
}

/* Length of a scheme at the start of s (letters, then letters/digits/+-.), then ':'; 0 if none. */
static size_t bm_url_scheme_len(const char *s, size_t n) {
    if (n == 0 || !((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z')) return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (c == ':') return i;
        if (!(((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) return 0;
    }
    return 0;
}

/* Appends `s` percent-encoding bytes a URL can't hold as-is in this part. */
static void bm_url_push_encoded(bm_sb *sb, const char *s, size_t n, bool query) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        bool enc = c <= 0x20 || c >= 0x7f || c == '"' || c == '<' || c == '>' || (!query && (c == '`' || c == '{' || c == '}')) || (query && c == '\'');
        if (enc) {
            char e[3] = { '%', hex[c >> 4], hex[c & 15] };
            bm_sb_push(sb, e, 3);
        } else {
            bm_sb_push_char(sb, (char)c);
        }
    }
}

/* Removes "." and ".." segments from an absolute path (in place in sb). */
static void bm_url_normalize_path(bm_sb *out, const char *p, size_t n) {
    /* segments stack as offsets into out */
    size_t starts[256];
    int depth = 0;
    out->len = 0;
    size_t i = 0;
    if (n == 0 || p[0] != '/') { bm_sb_push_char(out, '/'); }
    while (i < n) {
        if (p[i] == '/') i++;
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        size_t len = j - i;
        bool last = j >= n;
        bool dot = (len == 1 && p[i] == '.') || (len == 3 && (memcmp(p + i, "%2e", 3) == 0 || memcmp(p + i, "%2E", 3) == 0));
        bool dotdot = (len == 2 && p[i] == '.' && p[i + 1] == '.');
        if (dotdot) {
            if (depth > 0) out->len = starts[--depth];
            if (last) bm_sb_push_char(out, '/');
        } else if (dot) {
            if (last) bm_sb_push_char(out, '/');
        } else {
            if (depth < 256) starts[depth++] = (size_t)out->len;
            bm_sb_push_char(out, '/');
            bm_url_push_encoded(out, p + i, len, false);
        }
        i = j;
        if (i < n && p[i] == '/' && i + 1 == n) { /* trailing slash */
            bm_sb_push_char(out, '/');
            break;
        }
    }
    if (out->len == 0) bm_sb_push_char(out, '/');
}

typedef struct bm_url_parts {
    bm_sb proto, host, port, path, search, hash, userinfo;
    bool authority, has_query, has_hash;
} bm_url_parts;

static void bm_url_free(bm_url_parts *u) {
    bm_sb_free(&u->proto); bm_sb_free(&u->host); bm_sb_free(&u->port);
    bm_sb_free(&u->path); bm_sb_free(&u->search); bm_sb_free(&u->hash); bm_sb_free(&u->userinfo);
}

/* Parses an absolute URL; false if it isn't one. */
static bool bm_url_parse_abs(const char *s, size_t n, bm_url_parts *u) {
    size_t sl = bm_url_scheme_len(s, n);
    if (sl == 0) return false;
    for (size_t i = 0; i < sl; i++) bm_sb_push_char(&u->proto, (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]));
    bm_sb_push_char(&u->proto, ':');
    bool special = bm_url_special(u->proto.data, sl);
    size_t i = sl + 1;
    /* hash and query end the rest */
    size_t hash_at = n, query_at = n;
    for (size_t k = i; k < n; k++) if (s[k] == '#') { hash_at = k; break; }
    for (size_t k = i; k < hash_at; k++) if (s[k] == '?') { query_at = k; break; }
    size_t path_end = query_at;
    bool has_auth = (i + 1 < n && (s[i] == '/' || (special && s[i] == '\\')) && (s[i + 1] == '/' || (special && s[i + 1] == '\\')));
    if (has_auth) {
        u->authority = true;
        i += 2;
        size_t a = i;
        while (i < path_end && s[i] != '/' && !(special && s[i] == '\\')) i++;
        /* authority s[a..i): userinfo (kept for href), host, port */
        size_t at = a;
        for (size_t k = a; k < i; k++) if (s[k] == '@') at = k + 1;
        if (at > a) bm_url_push_encoded(&u->userinfo, s + a, at - 1 - a, false);
        size_t colon = i;
        if (at < i && s[at] == '[') {
            size_t rb = at;
            while (rb < i && s[rb] != ']') rb++;
            for (size_t k = rb; k < i; k++) if (s[k] == ':') { colon = k; break; }
        } else {
            for (size_t k = at; k < i; k++) if (s[k] == ':') { colon = k; break; }
        }
        for (size_t k = at; k < colon; k++) bm_sb_push_char(&u->host, (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]));
        if (special && u->host.len == 0) return false;
        if (colon < i) {
            size_t ps = colon + 1;
            for (size_t k = ps; k < i; k++) if (s[k] < '0' || s[k] > '9') return false;
            while (ps + 1 < i && s[ps] == '0') ps++; /* "080" → "80" */
            if (ps < i) {
                long port = strtol(s + ps, NULL, 10);
                if (port > 65535) return false;
                const char *def = bm_url_default_port(u->proto.data, sl);
                if (!def || (size_t)(i - ps) != strlen(def) || memcmp(s + ps, def, strlen(def)) != 0) bm_sb_push(&u->port, s + ps, i - ps);
            }
        }
    }
    if (special) {
        bm_url_normalize_path(&u->path, s + i, path_end - i);
    } else {
        bm_url_push_encoded(&u->path, s + i, path_end - i, false);
    }
    u->has_query = query_at < hash_at;
    u->has_hash = hash_at < n;
    if (query_at < hash_at && hash_at - query_at > 1) bm_url_push_encoded(&u->search, s + query_at, hash_at - query_at, true);
    if (hash_at < n && n - hash_at > 1) bm_url_push_encoded(&u->hash, s + hash_at, n - hash_at, false);
    return true;
}

/* The common case — `http(s)://host[:port]/path[?query][#hash]` with nothing to encode or
 * normalize: its part boundaries (no allocation). False: take the full parser. */
typedef struct bm_url_bounds { size_t scheme, host, host_end, port, port_end, path, query, hash; bool default_port; } bm_url_bounds;

static bool bm_url_scan(const char *s, size_t n, bm_url_bounds *b) {
    size_t sl = n >= 7 && memcmp(s, "http://", 7) == 0 ? 4 : n >= 8 && memcmp(s, "https://", 8) == 0 ? 5 : 0;
    if (!sl) return false;
    size_t h = sl + 3, i = h, colon = 0;
    for (; i < n && s[i] != '/' && s[i] != '?' && s[i] != '#'; i++) {
        char c = s[i];
        if (c == ':') colon = i;
        else if (c == '@' || c == '[' || c == '\\' || (c >= 'A' && c <= 'Z') || (unsigned char)c <= ' ' || (unsigned char)c >= 0x7f) return false;
    }
    size_t host_end = colon ? colon : i;
    if (host_end == h) return false;
    size_t port_at = colon ? colon + 1 : i;
    for (size_t k = port_at; k < i; k++) if (s[k] < '0' || s[k] > '9') return false;
    if (colon && (i - port_at == 0 || s[port_at] == '0' || i - port_at > 5)) return false;
    size_t path_at = i, q = n, hash = n;
    if (path_at == n || s[path_at] != '/') return false; /* "http://host" → path "/" */
    for (size_t k = path_at; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c <= ' ' || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '`' || c == '{' || c == '}' || c == '\\' || c == '\'') return false;
        if (c == '#') { hash = k; break; }
        if (c == '?' && q == n) q = k;
        if (c == '.' && q == n && s[k - 1] == '/') return false; /* dot segments: normalize */
        if (c == '%' && q == n) return false;                    /* %2e etc. */
    }
    b->scheme = sl;
    b->host = h;
    b->host_end = host_end;
    b->port = port_at;
    b->port_end = i;
    b->path = path_at;
    b->query = q < hash ? q : hash;
    b->hash = hash;
    b->default_port = colon && ((sl == 4 && i - port_at == 2 && memcmp(s + port_at, "80", 2) == 0) || (sl == 5 && i - port_at == 3 && memcmp(s + port_at, "443", 3) == 0));
    return true;
}

static bool bm_url_fast(const char *s, size_t n, bm_arr *out) {
    bm_url_bounds b;
    if (!bm_url_scan(s, n, &b)) return false;
    bm_str parts[6];
    parts[0] = bm_str_from(s, b.scheme + 1);
    parts[1] = bm_str_from(s + b.host, b.host_end - b.host);
    parts[2] = b.port < b.port_end && !b.default_port ? bm_str_from(s + b.port, b.port_end - b.port) : BM_EMPTY_STR;
    parts[3] = bm_str_from(s + b.path, b.query - b.path);
    parts[4] = b.hash - b.query > 1 ? bm_str_from(s + b.query, b.hash - b.query) : BM_EMPTY_STR;
    parts[5] = n - b.hash > 1 ? bm_str_from(s + b.hash, n - b.hash) : BM_EMPTY_STR;
    *out = bm_arr_with_capacity(&bm_type_str, 6);
    for (int k = 0; k < 6; k++) bm_arr_push(out, &bm_type_str, &parts[k]);
    return true;
}

/* Parses `input` (resolved against `base` when relative) into u; false if it isn't a URL. */
static bool bm_url_parse_full(bm_str input, bm_str base, bm_url_parts *u) {
    const char *s = input.p->data;
    size_t n = (size_t)input.p->len;
    while (n && (unsigned char)*s <= ' ') { s++; n--; }
    while (n && (unsigned char)s[n - 1] <= ' ') n--;
    memset(u, 0, sizeof *u);
    bool ok = bm_url_parse_abs(s, n, u);
    if (!ok && base.p->len) {
        bm_url_free(u);
        memset(u, 0, sizeof *u);
        bm_url_parts b = {0};
        if (bm_url_parse_abs(base.p->data, (size_t)base.p->len, &b)) {
            bm_sb abs = {0};
            bm_sb_push(&abs, b.proto.data, (size_t)b.proto.len);
            if (n >= 2 && s[0] == '/' && s[1] == '/') {
                bm_sb_push(&abs, s, n);
            } else {
                bm_sb_push(&abs, "//", 2);
                if (b.userinfo.len) { bm_sb_push(&abs, b.userinfo.data, (size_t)b.userinfo.len); bm_sb_push_char(&abs, '@'); }
                bm_sb_push(&abs, b.host.data, (size_t)b.host.len);
                if (b.port.len) { bm_sb_push_char(&abs, ':'); bm_sb_push(&abs, b.port.data, (size_t)b.port.len); }
                if (n && s[0] == '/') {
                    bm_sb_push(&abs, s, n);
                } else if (n && s[0] == '?') {
                    bm_sb_push(&abs, b.path.data, (size_t)b.path.len);
                    bm_sb_push(&abs, s, n);
                } else if (n && s[0] == '#') {
                    bm_sb_push(&abs, b.path.data, (size_t)b.path.len);
                    if (b.has_query) bm_sb_push_char(&abs, '?');
                    if (b.search.len > 1) bm_sb_push(&abs, b.search.data + 1, (size_t)b.search.len - 1);
                    bm_sb_push(&abs, s, n);
                } else if (n == 0) {
                    bm_sb_push(&abs, b.path.data, (size_t)b.path.len);
                    if (b.has_query) bm_sb_push_char(&abs, '?');
                    if (b.search.len > 1) bm_sb_push(&abs, b.search.data + 1, (size_t)b.search.len - 1);
                } else {
                    size_t dir = (size_t)b.path.len;
                    while (dir && b.path.data[dir - 1] != '/') dir--;
                    bm_sb_push(&abs, b.path.data, dir);
                    bm_sb_push(&abs, s, n);
                }
            }
            ok = bm_url_parse_abs(abs.data, (size_t)abs.len, u);
            bm_sb_free(&abs);
        }
        bm_url_free(&b);
    }
    if (!ok) bm_url_free(u);
    return ok;
}

bm_arr bm_native_urlParse(bm_str input, bm_str base) {
    bm_arr fast;
    if (bm_url_fast(input.p->data, (size_t)input.p->len, &fast)) return fast;
    bm_url_parts u;
    if (!bm_url_parse_full(input, base, &u)) return BM_EMPTY_ARR;
    bm_arr out = bm_arr_with_capacity(&bm_type_str, 6);
    bm_sb *parts[6] = { &u.proto, &u.host, &u.port, &u.path, &u.search, &u.hash };
    for (int k = 0; k < 6; k++) {
        bm_str v = bm_str_from_sb(parts[k]);
        bm_arr_push(&out, &bm_type_str, &v);
    }
    bm_url_free(&u);
    return out;
}

bm_str bm_native_urlDecode(bm_str s, bool plus) {
    const char *p = s.p->data;
    size_t n = (size_t)s.p->len;
    bool any = false;
    for (size_t i = 0; i < n; i++) if (p[i] == '%' || (plus && p[i] == '+')) { any = true; break; }
    if (!any) { bm_str_retain(s); return s; }
    bm_sb sb = {0};
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '+' && plus) { bm_sb_push_char(&sb, ' '); continue; }
        if (c == '%' && i + 2 < n) {
            int h, l;
            char a = p[i + 1], b = p[i + 2];
            h = a >= '0' && a <= '9' ? a - '0' : (a | 32) >= 'a' && (a | 32) <= 'f' ? (a | 32) - 'a' + 10 : -1;
            l = b >= '0' && b <= '9' ? b - '0' : (b | 32) >= 'a' && (b | 32) <= 'f' ? (b | 32) - 'a' + 10 : -1;
            if (h >= 0 && l >= 0) { bm_sb_push_char(&sb, (char)(h * 16 + l)); i += 2; continue; }
        }
        bm_sb_push_char(&sb, c);
    }
    return bm_str_from_sb(&sb);
}

bm_str bm_native_urlEncode(bm_str s) {
    static const char hex[] = "0123456789ABCDEF";
    bm_sb sb = {0};
    for (int64_t i = 0; i < s.p->len; i++) {
        unsigned char c = (unsigned char)s.p->data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_') bm_sb_push_char(&sb, (char)c);
        else if (c == ' ') bm_sb_push_char(&sb, '+');
        else { char e[3] = { '%', hex[c >> 4], hex[c & 15] }; bm_sb_push(&sb, e, 3); }
    }
    return bm_str_from_sb(&sb);
}

/* Normalized href of a URL ("" if it isn't one). An already-normal URL is returned as is. */
bm_str bm_native_urlNormalize(bm_str input, bm_str base) {
    const char *s = input.p->data;
    size_t n = (size_t)input.p->len;
    size_t lead = 0;
    while (lead < n && (unsigned char)s[lead] <= ' ') lead++;
    size_t trail = n;
    while (trail > lead && (unsigned char)s[trail - 1] <= ' ') trail--;
    bm_url_bounds b;
    /* already normal: no default port, no empty "?" or "#" */
    if (lead == 0 && trail == n && bm_url_scan(s, n, &b) && !b.default_port && b.hash - b.query != 1 && n - b.hash != 1) {
        bm_str_retain(input);
        return input;
    }
    bm_url_parts u;
    if (!bm_url_parse_full(input, base, &u)) return BM_EMPTY_STR;
    bm_sb sb = {0};
    bm_sb_push(&sb, u.proto.data, (size_t)u.proto.len);
    if (u.authority) {
        bm_sb_push(&sb, "//", 2);
        if (u.userinfo.len) { bm_sb_push(&sb, u.userinfo.data, (size_t)u.userinfo.len); bm_sb_push_char(&sb, '@'); }
        bm_sb_push(&sb, u.host.data, (size_t)u.host.len);
        if (u.port.len) { bm_sb_push_char(&sb, ':'); bm_sb_push(&sb, u.port.data, (size_t)u.port.len); }
    }
    bm_sb_push(&sb, u.path.data, (size_t)u.path.len);
    if (u.has_query) bm_sb_push_char(&sb, '?');
    if (u.search.len > 1) bm_sb_push(&sb, u.search.data + 1, (size_t)u.search.len - 1);
    if (u.has_hash) bm_sb_push_char(&sb, '#');
    if (u.hash.len > 1) bm_sb_push(&sb, u.hash.data + 1, (size_t)u.hash.len - 1);
    bm_url_free(&u);
    return bm_str_from_sb(&sb);
}

/* Part k of a normalized href: 0 protocol ("http:"), 1 hostname, 2 port, 3 pathname,
 * 4 search ("?q" or ""), 5 hash ("#h" or ""). */
bm_str bm_native_urlPart(bm_str href, bm_int k) {
    const char *s = href.p->data, *end = s + href.p->len;
    const char *colon = memchr(s, ':', (size_t)href.p->len);
    if (!colon) return BM_EMPTY_STR;
    if (k == 0) return bm_str_from(s, (size_t)(colon + 1 - s));
    const char *p = colon + 1, *host = p, *host_end = p, *port = p, *port_end = p;
    if (end - p >= 2 && p[0] == '/' && p[1] == '/') {
        host = p + 2;
        const char *a = host;
        while (a < end && *a != '/' && *a != '?' && *a != '#') a++;
        for (const char *x = host; x < a; x++) if (*x == '@') host = x + 1; /* userinfo */
        const char *c = host;
        if (c < a && *c == '[') { while (c < a && *c != ']') c++; }
        while (c < a && *c != ':') c++;
        host_end = c;
        port = c < a ? c + 1 : a;
        port_end = a;
        p = a;
    }
    if (k == 1) return bm_str_from(host, (size_t)(host_end - host));
    if (k == 2) return bm_str_from(port, (size_t)(port_end - port));
    const char *hash = memchr(p, '#', (size_t)(end - p));
    if (!hash) hash = end;
    const char *q = memchr(p, '?', (size_t)(hash - p));
    if (!q) q = hash;
    if (k == 3) return bm_str_from(p, (size_t)(q - p));
    if (k == 4) return hash - q > 1 ? bm_str_from(q, (size_t)(hash - q)) : BM_EMPTY_STR;
    return end - hash > 1 ? bm_str_from(hash, (size_t)(end - hash)) : BM_EMPTY_STR;
}

/* ================================================================== HTTP client (fetch) */

/* fetch() runs natively on the event loop: DNS lookups on a small thread pool (cached), pooled
 * keep-alive connections per origin, a request writer and an incremental HTTP/1.1 response
 * parser, redirects, and gzip/deflate decoding. The standard library (std/http) wraps it:
 * fetchStart begins a request and returns its id, fetchWait gives a Promise<int> that settles
 * once the whole response has arrived (0) or the request failed (< 0), and the other natives
 * read the result. Nothing here is referenced unless a program calls fetch. */

#include <limits.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/uio.h>
#include <sys/un.h>

/* ---- DNS: getaddrinfo on a few lazily started threads (64 KiB stacks), results cached */

#define BM_DNS_MAX_ADDRS 8
#define BM_DNS_TTL_MS 30000
typedef struct bm_addrs {
    int n;
    struct sockaddr_storage a[BM_DNS_MAX_ADDRS];
    socklen_t len[BM_DNS_MAX_ADDRS];
} bm_addrs;

struct bm_fr;
typedef struct bm_dns {
    struct bm_dns *next;       /* cache chain */
    struct bm_dns *job_next;   /* job or done queue */
    char *host;
    bool resolving;
    int err;                   /* getaddrinfo's error (0: found) */
    bm_addrs addrs;
    uint64_t expires;
    struct bm_fr *waiters;     /* requests waiting for this lookup */
} bm_dns;

#define BM_DNS_BUCKETS 64
static bm_dns *bm_dns_cache[BM_DNS_BUCKETS];
static pthread_mutex_t bm_dns_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bm_dns_cv = PTHREAD_COND_INITIALIZER;
static bm_dns *bm_dns_jobs, *bm_dns_done;
static int bm_dns_threads, bm_dns_idle;
static int bm_dns_pipe[2] = {-1, -1};

static uint32_t bm_hash_cstr(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void bm_addrs_from(bm_addrs *out, struct addrinfo *ai) {
    out->n = 0;
    /* IPv4 first: a server bound to 0.0.0.0 doesn't answer on ::1 */
    for (int pass = 0; pass < 2; pass++)
        for (struct addrinfo *a = ai; a && out->n < BM_DNS_MAX_ADDRS; a = a->ai_next) {
            if ((a->ai_family == AF_INET) != (pass == 0)) continue;
            if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
            memcpy(&out->a[out->n], a->ai_addr, a->ai_addrlen);
            out->len[out->n++] = a->ai_addrlen;
        }
}

static void *bm_dns_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&bm_dns_mu);
    for (;;) {
        while (!bm_dns_jobs) {
            bm_dns_idle++;
            pthread_cond_wait(&bm_dns_cv, &bm_dns_mu);
            bm_dns_idle--;
        }
        bm_dns *d = bm_dns_jobs;
        bm_dns_jobs = d->job_next;
        pthread_mutex_unlock(&bm_dns_mu);
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_ADDRCONFIG;
        const char *h = d->host;
        char bare[256];
        size_t hl = strlen(h);
        if (hl >= 2 && h[0] == '[' && h[hl - 1] == ']' && hl - 2 < sizeof bare) { memcpy(bare, h + 1, hl - 2); bare[hl - 2] = 0; h = bare; }
        int err = getaddrinfo(h, NULL, &hints, &res);
        if (!err) {
            bm_addrs_from(&d->addrs, res);
            if (d->addrs.n == 0) err = EAI_NONAME;
            freeaddrinfo(res);
        }
        d->err = err;
        pthread_mutex_lock(&bm_dns_mu);
        d->job_next = bm_dns_done;
        bm_dns_done = d;
        pthread_mutex_unlock(&bm_dns_mu);
        char one = 1;
        ssize_t w = write(bm_dns_pipe[1], &one, 1);
        (void)w;
        pthread_mutex_lock(&bm_dns_mu);
    }
    return NULL;
}

static void bm_dns_ready(bm_io *h, bool readable, bool writable, bool broken);
/* `ready` is set when the first lookup starts: a statically initialized code pointer here would
 * land in clang's merged globals next to the event loop's, and keep all of this linked. */
static bm_io bm_dns_io;

static void bm_io_add(int fd, bm_io *h, bool read, bool write) {
    int q = bm_loop_queue();
    void *tag = (void *)((uintptr_t)h | 1);
#ifdef BM_KQUEUE
    struct kevent ev[2];
    int n = 0;
    if (read) EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD, 0, 0, tag);
    if (write) EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, tag);
    kevent(q, ev, n, NULL, 0, NULL);
#else
    struct epoll_event ev = { .events = (read ? EPOLLIN | EPOLLRDHUP : 0) | (write ? EPOLLOUT : 0), .data.ptr = tag };
    epoll_ctl(q, EPOLL_CTL_ADD, fd, &ev);
#endif
}

static void bm_dns_start_thread(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 > PTHREAD_STACK_MIN ? 64 * 1024 : PTHREAD_STACK_MIN);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, bm_dns_worker, NULL) == 0) bm_dns_threads++;
    pthread_attr_destroy(&attr);
}

static void bm_dns_queue(bm_dns *d) {
    if (bm_dns_pipe[0] < 0) {
        bm_dns_io.ready = bm_dns_ready;
        if (pipe(bm_dns_pipe) != 0) bm_trap("can't create a pipe for DNS lookups", NULL);
        for (int i = 0; i < 2; i++) {
            fcntl(bm_dns_pipe[i], F_SETFL, fcntl(bm_dns_pipe[i], F_GETFL) | O_NONBLOCK);
            fcntl(bm_dns_pipe[i], F_SETFD, FD_CLOEXEC);
        }
        bm_io_add(bm_dns_pipe[0], &bm_dns_io, true, false);
    }
    bm_io_refs++;
    pthread_mutex_lock(&bm_dns_mu);
    d->job_next = bm_dns_jobs;
    bm_dns_jobs = d;
    if (bm_dns_idle == 0 && bm_dns_threads < 4) bm_dns_start_thread();
    else pthread_cond_signal(&bm_dns_cv);
    pthread_mutex_unlock(&bm_dns_mu);
}

/* ---- UTF-8 validation */

/* Validates UTF-8 from *pos up to n: false at the first invalid sequence; otherwise *pos stops at
 * the end, or at a sequence cut off by the end (more bytes may complete it). ASCII runs go 16
 * bytes at a time. */
static bool bm_utf8_scan(const uint8_t *p, size_t n, size_t *pos) {
    size_t i = *pos;
    while (i < n) {
        if (n - i >= 16) {
            uint64_t a, b;
            memcpy(&a, p + i, 8);
            memcpy(&b, p + i + 8, 8);
            if (!((a | b) & 0x8080808080808080ull)) { i += 16; continue; }
        }
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        size_t need = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
        if (!need) { *pos = i; return false; }
        if (n - i <= need) break;
        uint8_t lo = c == 0xE0 ? 0xA0 : c == 0xF0 ? 0x90 : 0x80, hi = c == 0xED ? 0x9F : c == 0xF4 ? 0x8F : 0xBF;
        if (p[i + 1] < lo || p[i + 1] > hi) { *pos = i; return false; }
        for (size_t k = 2; k <= need; k++)
            if ((p[i + k] & 0xC0) != 0x80) { *pos = i; return false; }
        i += need + 1;
    }
    *pos = i;
    return true;
}

/* ---- requests and connections */

typedef struct bm_fc bm_fc;
typedef struct bm_origin {
    struct bm_origin *next;
    char *key;                 /* "host:port" */
    bm_fc *idle;               /* idle connections, most recently used first */
    int nidle;
} bm_origin;

enum { BM_FR_HEAD, BM_FR_FIXED, BM_FR_CHUNK_SIZE, BM_FR_CHUNK_DATA, BM_FR_CHUNK_END, BM_FR_TRAILERS, BM_FR_UNTIL_CLOSE, BM_FR_DONE };
enum { BM_FETCH_FOLLOW, BM_FETCH_MANUAL, BM_FETCH_ERROR };
enum { BM_FETCH_DECOMPRESS = 1, BM_FETCH_INSECURE = 2 };

/* Set by bm_tls_install() in programs linked with TLS (runtime/tls.c, runtime/codecs.c). */
const bm_tls_ops *bm_tls_impl;
const bm_codec_ops *bm_codec;

typedef struct bm_fr {
    bm_int id;
    int result;                /* 1 running, 0 the whole response is in, < 0 failed */
    int head_state;            /* 1 waiting for the response head, 0 it arrived, < 0 failed first */
    bm_promise *done;          /* fetchWait: the head arrived (0) or the request failed */
    bm_promise *body_p;        /* fetchBodyWait: the body is in (0) or failed */
    bm_promise *read_p;        /* fetchRead: body bytes to take (> 0), the end (0), or failed */
    bool streaming;            /* the body is read in chunks (res.body) */
    bool paused;               /* ... and the reader is behind: the socket isn't read */
    int shares;                /* Response.clone()s that will take the body too */
    struct bm_fr *dns_next;    /* waiting on a lookup */
    /* the request */
    bm_str method, headers, body, url;
    int redirect_mode, redirects;
    bool decompress;
    bool https, insecure;      /* TLS; without certificate checks (tls.rejectUnauthorized: false) */
    bm_str ca;                 /* extra trusted certificates (PEM), for tls.ca */
    bm_str unix_path;          /* Bun's `unix`: connect to this socket instead ("" for TCP) */
    bm_str proxy;              /* the `proxy` option ("": HTTP_PROXY/HTTPS_PROXY, unless NO_PROXY) */
    /* where to connect: the host itself, or the proxy (then `via_proxy`) */
    char *dial_host;
    int dial_port;
    bool via_proxy;
    char *proxy_auth;          /* "Basic ..." for the proxy, or NULL */
    char *host;                /* hostname (IPv6 in brackets) */
    char *key;                 /* the pool key: scheme, host, port and TLS options */
    int port;
    bm_sb head;                /* the serialized request head */
    bool merged;               /* the body was appended to head (one TLS record for both) */
    size_t sent;               /* bytes of head + body written */
    bm_fc *c;
    bool retried;
    /* the response */
    int state, status;
    bool keep, discard, head_only, got_any;
    int enc;                   /* 1 gzip, 2 deflate, 3 br, 4 zstd */
    /* An encoded body (unless decompress: false) arrives in raw and is decoded onto rbody as it
     * does; a reader that's behind leaves some in raw (from raw_off), undecoded. */
    bm_decoder *dec;
    bm_sb raw;
    size_t raw_off;
    /* A body read in chunks (streaming) is decoded as it arrives, at most BM_STREAM_HIGH ahead of
     * the reader: a small body that decodes to a huge one is decoded as it's read. A body read
     * whole is decoded at once when it's all in (faster). Until the program reads it, nothing
     * is decoded; a body that has all arrived by then is draining (the request settles once
     * it's decoded). */
    bool whole, draining;
    int64_t remaining;
    /* The body is checked as UTF-8 while it arrives (still in cache): text() then needn't read
     * it again. A second pass over a big body right after it arrived also slows the next
     * transfer (it evicts what the kernel's copy needs). */
    size_t utf8_pos;
    bool utf8_bad;
    bm_sb status_text, rheaders, rbody, location;
    bool redirected;
    const char *code;          /* failure: Bun's error code (points into code_buf) */
    char code_buf[64];
    bm_sb message;
} bm_fr;

struct bm_fc {
    bm_io io;
    int fd;
    bool reading, writing, registered;
    bool connecting, reused, dead;
    bm_origin *origin;
    bm_fc *next;               /* idle list, or the list of closed handles */
    bm_fr *r;                  /* the request in progress (NULL when idle) */
    bm_addrs addrs;            /* where to connect */
    int ai;                    /* the address being tried */
    int err;                   /* the last connect error */
    int port;
    bm_tls *tls;               /* https: the TLS session (NULL for http) */
    int early_err;             /* the ClientHello, sent while connecting, failed with this */
    bool handshaking;
    bool tunneling;            /* waiting for a proxy's answer to CONNECT */
    bool no_reuse;             /* a proxy refused the tunnel: its connection isn't ours to keep */
    bm_sb ctl;                 /* the CONNECT request, while it's being sent */
    size_t ctl_off;
    char *in;
    size_t in_off, in_len, in_cap;
};

#define BM_ORIGIN_BUCKETS 64
static bm_origin *bm_origins[BM_ORIGIN_BUCKETS];
static bm_fc *bm_fc_graveyard;
/* Requests by id: slot + 1 in the low 32 bits, a generation above (stale ids find nothing). */
static bm_fr **bm_fr_table;
static bm_int *bm_fr_free;          /* free slots */
static bm_int bm_fr_cap, bm_fr_nfree;
static uint32_t bm_fr_gen;

static void bm_fr_step(bm_fr *r);
static void bm_fc_ready(bm_io *h, bool readable, bool writable, bool broken);

static void bm_fc_reap(void) {
    while (bm_fc_graveyard) {
        bm_fc *c = bm_fc_graveyard;
        bm_fc_graveyard = c->next;
        bm_sb_free(&c->ctl);
        bm_free(c->in);
        bm_free(c);
    }
}

static void bm_fc_interest(bm_fc *c, bool read, bool write) {
    if (c->registered && read == c->reading && write == c->writing) return;
    int q = bm_loop_queue();
    void *tag = (void *)((uintptr_t)&c->io | 1);
#ifdef BM_KQUEUE
    struct kevent ev[2];
    int n = 0;
    if (!c->registered || read != c->reading) EV_SET(&ev[n++], c->fd, EVFILT_READ, !c->registered ? (read ? EV_ADD : EV_ADD | EV_DISABLE) : read ? EV_ENABLE : EV_DISABLE, 0, 0, tag);
    if (write != c->writing) EV_SET(&ev[n++], c->fd, EVFILT_WRITE, write ? EV_ADD : EV_DELETE, 0, 0, tag);
    if (n) kevent(q, ev, n, NULL, 0, NULL);
#else
    struct epoll_event ev = { .events = (read ? EPOLLIN | EPOLLRDHUP : 0) | (write ? EPOLLOUT : 0), .data.ptr = tag };
    epoll_ctl(q, c->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, c->fd, &ev);
#endif
    c->registered = true;
    c->reading = read;
    c->writing = write;
}

static void bm_fc_close(bm_fc *c) {
    if (c->dead) return;
    c->dead = true;
    if (c->tls) { bm_tls_impl->close(c->tls, false); c->tls = NULL; }
    if (c->fd >= 0) close(c->fd); /* also leaves the event queue */
    c->fd = -1;
    c->next = bm_fc_graveyard;
    bm_fc_graveyard = c;
}

static bm_origin *bm_origin_get(const char *key) {
    uint32_t b = bm_hash_cstr(key) % BM_ORIGIN_BUCKETS;
    for (bm_origin *o = bm_origins[b]; o; o = o->next)
        if (strcmp(o->key, key) == 0) return o;
    bm_origin *o = bm_alloc(sizeof *o);
    memset(o, 0, sizeof *o);
    size_t n = strlen(key);
    o->key = bm_alloc(n + 1);
    memcpy(o->key, key, n + 1);
    o->next = bm_origins[b];
    bm_origins[b] = o;
    return o;
}

static void bm_origin_unlink(bm_fc *c) {
    bm_origin *o = c->origin;
    for (bm_fc **pp = &o->idle; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; o->nidle--; return; }
}

/* ---- failure */

static void bm_fr_resolve(bm_promise **pp, bm_int v) {
    bm_promise *p = *pp;
    if (!p) return;
    *pp = NULL;
    bm_promise_resolve(p, &v);
    bm_promise_release(p);
}

/* The response is complete (0), or the request failed (< 0) — before the head arrived, or while
 * the body did. */
static void bm_fr_settle(bm_fr *r, int result) {
    if (r->result != 1) return;
    r->result = result;
    bm_io_refs--;
    if (r->head_state == 1) {
        r->head_state = result;
        bm_fr_resolve(&r->done, result);
    }
    bm_fr_resolve(&r->body_p, result);
    bm_fr_resolve(&r->read_p, result == 0 && r->rbody.len ? (bm_int)r->rbody.len : result);
}

/* The final response's head is in: fetch() resolves (the body follows). */
static void bm_fr_head_ready(bm_fr *r) {
    if (r->head_state != 1) return;
    r->head_state = 0;
    bm_fr_resolve(&r->done, 0);
}

/* A body streamed as it arrives: at most this much waits for the reader before the socket pauses. */
#define BM_STREAM_HIGH (4 << 20)

/* An encoded body that's decoded (not `decompress: false`). */
static inline bool bm_fr_decoding(bm_fr *r) { return r->decompress && r->enc; }

/* Where arriving body bytes go: an encoded body's are decoded from raw onto rbody. */
static inline bm_sb *bm_fr_sink(bm_fr *r) { return bm_fr_decoding(r) ? &r->raw : &r->rbody; }

static bool bm_fr_decode(bm_fr *r, bool final);

/* Body bytes arrived: decode them, and a waiting reader gets them. True when the reader is far
 * enough behind that the socket should pause. */
static bool bm_fr_data(bm_fr *r) {
    if (!r->streaming) return false;
    if (bm_fr_decoding(r) && !bm_fr_decode(r, false)) return false;
    if (r->read_p && r->rbody.len) bm_fr_resolve(&r->read_p, (bm_int)r->rbody.len);
    return r->rbody.len >= BM_STREAM_HIGH || r->raw_off < r->raw.len;
}

static void bm_fr_fail(bm_fr *r, const char *code, const char *fmt, ...) {
    if (r->result != 1) return;
    snprintf(r->code_buf, sizeof r->code_buf, "%s", code); /* the TLS layer's codes don't outlive the connection */
    r->code = r->code_buf;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    bm_sb_push(&r->message, buf, n < 0 ? 0 : (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    if (r->c) {
        bm_fc *c = r->c;
        r->c = NULL;
        c->r = NULL;
        bm_fc_close(c);
    }
    bm_fr_settle(r, -1);
}

static const char BM_FETCH_VERBOSE[] = "For more information, pass `verbose: true` in the second argument to fetch()";

/* ---- the request line and headers */

static bool bm_hdr_has(bm_str block, const char *name) { return bm_has_header(block, name); }

static void bm_base64(bm_sb *sb, const char *s, size_t n) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)s[i] << 16 | (i + 1 < n ? (uint32_t)(uint8_t)s[i + 1] << 8 : 0) | (i + 2 < n ? (uint8_t)s[i + 2] : 0);
        bm_sb_push_char(sb, tbl[v >> 18 & 63]);
        bm_sb_push_char(sb, tbl[v >> 12 & 63]);
        bm_sb_push_char(sb, i + 1 < n ? tbl[v >> 6 & 63] : '=');
        bm_sb_push_char(sb, i + 2 < n ? tbl[v & 63] : '=');
    }
}

/* NO_PROXY: "*", or comma-separated hosts, domains ("example.com" also covers its subdomains,
 * as does ".example.com") and host:port pairs. */
static bool bm_no_proxy(const char *host, int port) {
    const char *list = getenv("no_proxy");
    if (!list || !*list) list = getenv("NO_PROXY");
    if (!list) return false;
    size_t hl = strlen(host);
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *te = e;
        while (te > p && te[-1] == ' ') te--;
        size_t n = (size_t)(te - p);
        if (n == 1 && *p == '*') return true;
        const char *colon = memchr(p, ':', n);
        if (colon && !memchr(p, '[', n)) {
            if (atoi(colon + 1) != port) { p = e; continue; }
            n = (size_t)(colon - p);
        }
        const char *d = p;
        if (n && *d == '.') { d++; n--; }
        if (n && (hl == n || (hl > n && host[hl - n - 1] == '.')) && strncasecmp(host + hl - n, d, n) == 0) return true;
        p = e;
    }
    return false;
}

/* Where r connects: its host, or (the `proxy` option, else HTTP_PROXY/HTTPS_PROXY unless
 * NO_PROXY) a proxy. Unix sockets never go through a proxy. */
static bool bm_fr_choose_proxy(bm_fr *r, bool https) {
    bm_free(r->dial_host);
    bm_free(r->proxy_auth);
    r->dial_host = NULL;
    r->proxy_auth = NULL;
    r->via_proxy = false;
    const char *spec = r->proxy.p->len ? r->proxy.p->data : NULL;
    if (!spec && !r->unix_path.p->len && !bm_no_proxy(r->host, r->port)) {
        spec = https ? getenv("https_proxy") : getenv("http_proxy");
        if (!spec || !*spec) spec = https ? getenv("HTTPS_PROXY") : getenv("HTTP_PROXY");
        if (spec && !*spec) spec = NULL;
    }
    if (!spec || r->unix_path.p->len) {
        size_t hl = strlen(r->host);
        r->dial_host = bm_alloc(hl + 1);
        memcpy(r->dial_host, r->host, hl + 1);
        r->dial_port = r->port;
        return true;
    }
    /* [http://][user:pass@]host[:port] */
    const char *p = spec;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strstr(p, "://")) { bm_fr_fail(r, "UnsupportedProxyProtocol", "Only http: proxies are supported: %s", spec); return false; }
    const char *end = p + strcspn(p, "/?#");
    const char *at = NULL;
    for (const char *x = p; x < end; x++) if (*x == '@') at = x;
    if (at) {
        bm_str ui = bm_native_urlDecode(bm_str_from(p, (size_t)(at - p)), false);
        bm_sb auth = {0};
        bm_sb_push(&auth, "Basic ", 6);
        bm_base64(&auth, ui.p->data, (size_t)ui.p->len);
        bm_sb_push_char(&auth, 0);
        r->proxy_auth = bm_alloc(auth.len);
        memcpy(r->proxy_auth, auth.data, auth.len);
        bm_sb_free(&auth);
        bm_str_release(ui);
        p = at + 1;
    }
    const char *hend = p;
    if (*hend == '[') { while (hend < end && *hend != ']') hend++; if (hend < end) hend++; }
    while (hend < end && *hend != ':') hend++;
    size_t hl = (size_t)(hend - p);
    if (hl == 0) { bm_fr_fail(r, "InvalidProxyURL", "The proxy URL is invalid: %s", spec); return false; }
    r->dial_host = bm_alloc(hl + 1);
    memcpy(r->dial_host, p, hl);
    r->dial_host[hl] = 0;
    r->dial_port = hend < end ? atoi(hend + 1) : 80;
    r->via_proxy = true;
    return true;
}

/* Splits r->url (a normalized href) into host, port and target and writes the request head.
 * False (after failing r) if it isn't an http URL. */
static bool bm_fr_prepare(bm_fr *r) {
    const char *s = r->url.p->data, *end = s + r->url.p->len;
    bool https = r->url.p->len >= 8 && memcmp(s, "https://", 8) == 0;
    if (!https && !(r->url.p->len >= 7 && memcmp(s, "http://", 7) == 0)) {
        bm_fr_fail(r, "ERR_INVALID_ARG_VALUE", "protocol must be http: or https:");
        return false;
    }
    if (https && !bm_tls_impl) {
        bm_fr_fail(r, "ERR_TLS_UNSUPPORTED", "https: isn't available: this program was built without TLS");
        return false;
    }
    r->https = https;
    const char *a = s + (https ? 8 : 7), *auth_end = a;
    while (auth_end < end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#') auth_end++;
    const char *host = a, *userinfo = NULL, *userinfo_end = NULL;
    for (const char *x = a; x < auth_end; x++) if (*x == '@') { userinfo = a; userinfo_end = x; host = x + 1; }
    const char *hend = host;
    if (hend < auth_end && *hend == '[') { while (hend < auth_end && *hend != ']') hend++; if (hend < auth_end) hend++; }
    while (hend < auth_end && *hend != ':') hend++;
    int port = https ? 443 : 80;
    if (hend < auth_end) port = atoi(hend + 1);
    bm_free(r->host);
    bm_free(r->key);
    size_t hl = (size_t)(hend - host);
    r->host = bm_alloc(hl + 1);
    memcpy(r->host, host, hl);
    r->host[hl] = 0;
    r->port = port;
    /* connections are pooled per scheme, host, port and TLS options */
    size_t kl = hl + 64;
    r->key = bm_alloc(kl);
    if (https) snprintf(r->key, kl, "https://%s:%d%s|%08x", r->host, port, r->insecure ? "|insecure" : "", r->ca.p->len ? bm_hash_cstr(r->ca.p->data) : 0);
    else snprintf(r->key, kl, "http://%s:%d", r->host, port);
    if (r->unix_path.p->len) { /* pooled per socket too */
        size_t used = strlen(r->key);
        snprintf(r->key + used, kl - used, "|unix:%08x", bm_hash_cstr(r->unix_path.p->data));
    }
    const char *target = auth_end, *hash = memchr(target, '#', (size_t)(end - target));
    if (!hash) hash = end;
    if (!bm_fr_choose_proxy(r, https)) return false;
    bm_sb *h = &r->head;
    h->len = 0;
    r->merged = false;
    bm_sb_push(h, r->method.p->data, (size_t)r->method.p->len);
    bm_sb_push_char(h, ' ');
    if (r->via_proxy && !https) {
        /* through a proxy, plain HTTP names the whole URL (absolute form) */
        bm_sb_push(h, s, (size_t)(auth_end - s));
    }
    if (target == hash || *target != '/') bm_sb_push_char(h, '/');
    bm_sb_push(h, target, (size_t)(hash - target));
    bm_sb_push(h, " HTTP/1.1\r\n", 11);
    bm_sb_push(h, r->headers.p->data, (size_t)r->headers.p->len);
    /* Bun's defaults, after the caller's headers */
    if (!bm_hdr_has(r->headers, "connection")) bm_sb_push(h, "Connection: keep-alive\r\n", 24);
    if (!bm_hdr_has(r->headers, "user-agent")) bm_sb_push(h, "User-Agent: Barm/0.0.1\r\n", 24);
    if (!bm_hdr_has(r->headers, "accept")) bm_sb_push(h, "Accept: */*\r\n", 13);
    if (!bm_hdr_has(r->headers, "host")) {
        bm_sb_push(h, "Host: ", 6);
        bm_sb_push(h, host, (size_t)(auth_end - host));
        bm_sb_push(h, "\r\n", 2);
    }
    /* what Bun asks for (brotli and zstd come with the TLS archive, which every fetch program links) */
    if (r->decompress && !bm_hdr_has(r->headers, "accept-encoding")) {
        if (bm_codec) bm_sb_push(h, "Accept-Encoding: gzip, deflate, br, zstd\r\n", 42);
    }
    if (r->via_proxy && !https && r->proxy_auth && !bm_hdr_has(r->headers, "proxy-authorization")) {
        bm_sb_push(h, "Proxy-Authorization: ", 21);
        bm_sb_push(h, r->proxy_auth, strlen(r->proxy_auth));
        bm_sb_push(h, "\r\n", 2);
    }
    if (userinfo && !bm_hdr_has(r->headers, "authorization")) {
        bm_str ui = bm_native_urlDecode(bm_str_from(userinfo, (size_t)(userinfo_end - userinfo)), false);
        bm_sb_push(h, "Authorization: Basic ", 21);
        bm_base64(h, ui.p->data, (size_t)ui.p->len);
        bm_sb_push(h, "\r\n", 2);
        bm_str_release(ui);
    }
    const char *m = r->method.p->data;
    /* as Bun: every method but GET and HEAD says its length, even when it's 0 */
    bool wants_length = r->body.p->len > 0 || (strcmp(m, "GET") != 0 && strcmp(m, "HEAD") != 0);
    if (wants_length && !bm_hdr_has(r->headers, "content-length") && !bm_hdr_has(r->headers, "transfer-encoding")) {
        char cl[48];
        int n = snprintf(cl, sizeof cl, "Content-Length: %d\r\n", (int)r->body.p->len);
        bm_sb_push(h, cl, (size_t)n);
    }
    bm_sb_push(h, "\r\n", 2);
    r->head_only = strcmp(m, "HEAD") == 0;
    if (r->via_proxy) { /* pooled per proxy too */
        size_t used = strlen(r->key), kl = used + strlen(r->dial_host) + 32;
        r->key = bm_realloc(r->key, kl);
        snprintf(r->key + used, kl - used, "|proxy:%s:%d", r->dial_host, r->dial_port);
    }
    return true;
}

/* ---- writing */

/* Bytes of request still to write: the head, then the body (unless merged into the head). */
static size_t bm_fr_out_len(const bm_fr *r) { return r->head.len + (r->merged ? 0 : (size_t)r->body.p->len); }

static bool bm_fc_write(bm_fc *c) {
    bm_fr *r = c->r;
    size_t hn = r->head.len, bn = r->merged ? 0 : (size_t)r->body.p->len;
    if (c->tls) {
        while (r->sent < hn + bn) {
            const char *p = r->sent < hn ? r->head.data + r->sent : r->body.p->data + (r->sent - hn);
            size_t n = r->sent < hn ? hn - r->sent : bn - (r->sent - hn);
            long w = bm_tls_impl->write(c->tls, p, n);
            if (w > 0) { r->sent += (size_t)w; continue; }
            if (w == -1) { bm_fc_interest(c, true, false); return true; }
            if (w == -2) { bm_fc_interest(c, true, true); return true; }
            return false;
        }
        bm_fc_interest(c, true, false);
        return true;
    }
    while (r->sent < hn + bn) {
        struct iovec iov[2];
        int n = 0;
        if (r->sent < hn) iov[n++] = (struct iovec){ r->head.data + r->sent, hn - r->sent };
        size_t bo = r->sent > hn ? r->sent - hn : 0;
        if (bn > bo) iov[n++] = (struct iovec){ r->body.p->data + bo, bn - bo };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = iov;
        mh.msg_iovlen = n;
#ifdef MSG_NOSIGNAL
        ssize_t w = sendmsg(c->fd, &mh, MSG_NOSIGNAL);
#else
        ssize_t w = sendmsg(c->fd, &mh, 0);
#endif
        if (w > 0) { r->sent += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { bm_fc_interest(c, true, true); return true; }
        return false;
    }
    bm_fc_interest(c, true, false);
    return true;
}

/* ---- the response */

static int bm_hex(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }

static bool bm_ieq(const char *a, size_t n, const char *lit) {
    size_t m = strlen(lit);
    if (n != m) return false;
    for (size_t i = 0; i < n; i++) if ((a[i] | 0x20) != lit[i]) return false;
    return true;
}

/* A token list header ("Connection: keep-alive, Upgrade") holds `word`. */
static bool bm_has_token(const char *v, size_t n, const char *word) {
    size_t wl = strlen(word);
    for (size_t i = 0; i < n;) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < n && v[i] != ',') i++;
        size_t e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        if (e - s == wl) {
            size_t k = 0;
            while (k < wl && (v[s + k] | 0x20) == word[k]) k++;
            if (k == wl) return true;
        }
    }
    return false;
}

static bool bm_redirect_status(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

/* Parses a response head (status line and header lines, `n` bytes up to the blank line). */
static bool bm_fr_head(bm_fr *r, const char *p, size_t n) {
    const char *end = p + n, *eol = memchr(p, '\n', n);
    if (!eol || eol - p < 12 || memcmp(p, "HTTP/1.", 7) != 0 || (p[7] != '0' && p[7] != '1') || p[8] != ' ') return false;
    bool http10 = p[7] == '0';
    if (p[9] < '1' || p[9] > '9' || p[10] < '0' || p[10] > '9' || p[11] < '0' || p[11] > '9') return false;
    r->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
    const char *reason = p + 12, *reason_end = eol;
    if (reason_end > reason && reason_end[-1] == '\r') reason_end--;
    if (reason < reason_end && *reason == ' ') reason++;
    r->status_text.len = 0;
    bm_sb_push(&r->status_text, reason, (size_t)(reason_end - reason));
    r->rheaders.len = 0;
    r->location.len = 0;
    r->enc = 0;
    int64_t length = -1;
    bool chunked = false, te = false, close = http10, keepalive_seen = false;
    for (const char *l = eol + 1; l < end;) {
        const char *le = memchr(l, '\n', (size_t)(end - l));
        if (!le) le = end;
        const char *lend = le > l && le[-1] == '\r' ? le - 1 : le;
        if (lend == l) break;
        const char *colon = memchr(l, ':', (size_t)(lend - l));
        if (!colon || colon == l) return false;
        for (const char *x = l; x < colon; x++) if ((unsigned char)*x <= ' ' || *x == 0x7f) return false;
        const char *v = colon + 1, *ve = lend;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t nl = (size_t)(colon - l), vl = (size_t)(ve - v);
        bm_sb_push(&r->rheaders, l, nl);
        bm_sb_push(&r->rheaders, ": ", 2);
        bm_sb_push(&r->rheaders, v, vl);
        bm_sb_push(&r->rheaders, "\r\n", 2);
        if (bm_ieq(l, nl, "content-length")) {
            int64_t cl = 0;
            if (vl == 0) return false;
            for (size_t i = 0; i < vl; i++) {
                if (v[i] < '0' || v[i] > '9' || cl > (INT64_MAX - 9) / 10) return false;
                cl = cl * 10 + (v[i] - '0');
            }
            if (length >= 0 && length != cl) return false;
            length = cl;
        } else if (bm_ieq(l, nl, "transfer-encoding")) {
            te = true;
            /* chunked must be the last coding */
            size_t k = vl;
            while (k > 0 && v[k - 1] != ',') k--;
            const char *last = v + k;
            while (last < ve && (*last == ' ' || *last == '\t')) last++;
            chunked = bm_ieq(last, (size_t)(ve - last), "chunked");
        } else if (bm_ieq(l, nl, "connection")) {
            if (bm_has_token(v, vl, "close")) close = true;
            if (bm_has_token(v, vl, "keep-alive")) keepalive_seen = true;
        } else if (bm_ieq(l, nl, "content-encoding")) {
            r->enc = bm_ieq(v, vl, "gzip") || bm_ieq(v, vl, "x-gzip") ? 1 : bm_ieq(v, vl, "deflate") ? 2 : bm_ieq(v, vl, "br") ? 3 : bm_ieq(v, vl, "zstd") ? 4 : 0;
        } else if (bm_ieq(l, nl, "location")) {
            r->location.len = 0;
            bm_sb_push(&r->location, v, vl);
        }
        l = le + 1;
    }
    if (http10 && keepalive_seen) close = false;
    r->keep = !close;
    int s = r->status;
    bool no_body = r->head_only || s == 204 || s == 304 || (s >= 100 && s < 200);
    if (no_body) {
        r->state = BM_FR_DONE;
    } else if (te) {
        if (chunked) r->state = BM_FR_CHUNK_SIZE;
        else { r->state = BM_FR_UNTIL_CLOSE; r->keep = false; }
    } else if (length >= 0) {
        r->remaining = length;
        r->state = length ? BM_FR_FIXED : BM_FR_DONE;
    } else {
        r->state = BM_FR_UNTIL_CLOSE;
        r->keep = false;
    }
    r->discard = r->redirect_mode != BM_FETCH_MANUAL && bm_redirect_status(s) && r->location.len > 0;
    if (!r->discard && length > 0 && r->state == BM_FR_FIXED) {
        if (length > INT32_MAX) return false;
        bm_sb_grow(bm_fr_sink(r), (size_t)(length < (64 << 20) ? length : (64 << 20)));
    }
    return true;
}

/* (an encoded body that isn't decoded is checked when text() reads it) */
static void bm_fr_check_utf8(bm_fr *r) {
    if (!r->utf8_bad && (!r->enc || bm_fr_decoding(r))) r->utf8_bad = !bm_utf8_scan((const uint8_t *)r->rbody.data, r->rbody.len, &r->utf8_pos);
}

/* Decodes what has arrived of an encoded body onto rbody: all of it at the end (final) or for a
 * whole-body read, else as much as a reader may have waiting. False (the request failed) if it's
 * corrupt, or at the end, cut short. */
static bool bm_fr_decode(bm_fr *r, bool final) {
    const char *why = r->enc == 3 ? "BrotliDecompressionError" : r->enc == 4 ? "ZstdDecompressionError" : "ZlibError"; /* Bun's codes */
    if (!r->dec && !(bm_codec && (r->dec = bm_codec->open(r->enc)))) {
        bm_fr_fail(r, why, "%s fetching \"%s\". %s", why, r->url.p->data, BM_FETCH_VERBOSE);
        return false;
    }
    size_t limit = 0;
    if (!final) {
        if (r->rbody.len >= BM_STREAM_HIGH) return true;
        limit = BM_STREAM_HIGH - r->rbody.len;
    }
    if (r->rbody.cap == 0) {
        /* one buffer for the output, reserved once: 8 times the encoded size when it's known
         * (pages never written cost nothing), at most what may wait for a reader. Growing it
         * step by step instead leaves a freed block per step (macOS keeps them cached). */
        size_t est = r->state == BM_FR_FIXED || r->state == BM_FR_DONE ? (r->raw.len + (size_t)r->remaining) * 8 : 1 << 20;
        if (limit && est > BM_STREAM_HIGH + (64 << 10)) est = BM_STREAM_HIGH + (64 << 10);
        if (est < (64 << 10)) est = 64 << 10;
        if (est > (1 << 30)) est = 1 << 30;
        bm_sb_grow(&r->rbody, est);
    }
    int st = bm_codec->step(r->dec, (const uint8_t *)r->raw.data, r->raw.len, &r->raw_off, &r->rbody, limit);
    if (st < 0 || (final && st == 0) || r->rbody.len > INT32_MAX) {
        /* corrupt, or (at the end) cut short */
        bm_fr_fail(r, why, "%s fetching \"%s\". %s", why, r->url.p->data, BM_FETCH_VERBOSE);
        return false;
    }
    if (r->raw_off == r->raw.len) {
        r->raw.len = r->raw_off = 0;
    } else if (r->raw_off >= (64 << 10)) {
        memmove(r->raw.data, r->raw.data + r->raw_off, r->raw.len - r->raw_off);
        r->raw.len -= r->raw_off;
        r->raw_off = 0;
    }
    bm_fr_check_utf8(r);
    return true;
}

/* Decodes an encoded body that has all arrived, at once. False (the request failed) if it's
 * corrupt or cut short. */
static bool bm_fr_decode_whole(bm_fr *r) {
    if (r->dec) return bm_fr_decode(r, true); /* a reader started it in chunks */
    if (!bm_codec || !bm_codec->whole(r->enc, (const uint8_t *)r->raw.data + r->raw_off, r->raw.len - r->raw_off, &r->rbody) || r->rbody.len > INT32_MAX) {
        const char *why = r->enc == 3 ? "BrotliDecompressionError" : r->enc == 4 ? "ZstdDecompressionError" : "ZlibError";
        bm_fr_fail(r, why, "%s fetching \"%s\". %s", why, r->url.p->data, BM_FETCH_VERBOSE);
        return false;
    }
    r->raw.len = r->raw_off = 0;
    bm_fr_check_utf8(r);
    return true;
}

static void bm_fr_take(bm_fr *r, const char *p, size_t n) {
    if (r->discard || r->result != 1) return;
    bm_sb_push(bm_fr_sink(r), p, n);
    if (!bm_fr_decoding(r)) bm_fr_check_utf8(r);
    bm_fr_data(r);
}

/* Consumes what it can of the connection's input. 1: the response is complete, 0: needs more,
 * -1: malformed. */
static int bm_fr_parse(bm_fr *r, bm_fc *c) {
    for (;;) {
        char *p = c->in + c->in_off;
        size_t n = c->in_len - c->in_off;
        switch (r->state) {
        case BM_FR_HEAD: {
            if (n == 0) return 0;
            /* the blank line ends the head */
            const char *e = NULL;
            for (const char *x = p; (x = memchr(x, '\n', (size_t)(p + n - x))) != NULL; x++) {
                if (x + 1 < p + n && x[1] == '\n') { e = x + 2; break; }
                if (x + 2 < p + n && x[1] == '\r' && x[2] == '\n') { e = x + 3; break; }
            }
            if (!e) return n > BM_HTTP_MAX_HEAD ? -1 : 0;
            if (!bm_fr_head(r, p, (size_t)(e - p))) return -1;
            c->in_off += (size_t)(e - p);
            if (r->status >= 100 && r->status < 200) {
                if (r->status == 101) return -1;
                r->state = BM_FR_HEAD; /* an interim response: the real one follows */
                continue;
            }
            if (!r->discard) bm_fr_head_ready(r);
            if (r->state == BM_FR_DONE) return 1;
            continue;
        }
        case BM_FR_FIXED: {
            if (n == 0) return 0;
            size_t take = (int64_t)n < r->remaining ? n : (size_t)r->remaining;
            bm_fr_take(r, p, take);
            if (r->result != 1) return 0; /* the body couldn't be decoded */
            c->in_off += take;
            r->remaining -= (int64_t)take;
            if (r->remaining == 0) { r->state = BM_FR_DONE; return 1; }
            continue;
        }
        case BM_FR_CHUNK_SIZE: {
            char *nl = memchr(p, '\n', n);
            if (!nl) return n > 4096 ? -1 : 0;
            int64_t size = 0;
            const char *x = p;
            int digits = 0;
            for (int d; (d = bm_hex(*x)) >= 0; x++, digits++) {
                if (size > (INT64_MAX >> 4)) return -1;
                size = size * 16 + d;
            }
            if (!digits) return -1;
            c->in_off += (size_t)(nl + 1 - p);
            if (size == 0) { r->state = BM_FR_TRAILERS; continue; }
            if (!r->discard && (int64_t)bm_fr_sink(r)->len + size > INT32_MAX) return -1;
            r->remaining = size;
            r->state = BM_FR_CHUNK_DATA;
            continue;
        }
        case BM_FR_CHUNK_DATA: {
            if (n == 0) return 0;
            size_t take = (int64_t)n < r->remaining ? n : (size_t)r->remaining;
            bm_fr_take(r, p, take);
            if (r->result != 1) return 0; /* the body couldn't be decoded */
            c->in_off += take;
            r->remaining -= (int64_t)take;
            if (r->remaining == 0) r->state = BM_FR_CHUNK_END;
            continue;
        }
        case BM_FR_CHUNK_END: {
            if (n == 0) return 0;
            if (p[0] == '\n') { c->in_off += 1; r->state = BM_FR_CHUNK_SIZE; continue; }
            if (n < 2) return 0;
            if (p[0] != '\r' || p[1] != '\n') return -1;
            c->in_off += 2;
            r->state = BM_FR_CHUNK_SIZE;
            continue;
        }
        case BM_FR_TRAILERS: {
            char *nl = memchr(p, '\n', n);
            if (!nl) return n > BM_HTTP_MAX_HEAD ? -1 : 0;
            bool blank = nl == p || (nl == p + 1 && p[0] == '\r');
            c->in_off += (size_t)(nl + 1 - p);
            if (blank) { r->state = BM_FR_DONE; return 1; }
            continue;
        }
        case BM_FR_UNTIL_CLOSE: {
            if (n) { bm_fr_take(r, p, n); c->in_off += n; }
            if (r->result != 1) return 0;
            if (!r->discard && bm_fr_sink(r)->len > INT32_MAX) return -1;
            return 0;
        }
        default:
            return 1;
        }
    }
}

/* ---- connections */

static void bm_fc_release(bm_fc *c, bool reusable) {
    c->r = NULL;
    if (!reusable || c->dead || c->no_reuse) { bm_fc_close(c); return; }
    bm_origin *o = c->origin;
    if (o->nidle >= 256) { bm_fc_close(c); return; }
    c->reused = true;
    c->in_off = c->in_len = 0;
    if (c->in_cap > 16384) { bm_free(c->in); c->in = NULL; c->in_cap = 0; }
    /* idle: watch for the server closing it */
    bm_fc_interest(c, true, false);
    c->next = o->idle;
    o->idle = c;
    o->nidle++;
}

/* > 0 bytes; 0 the connection ended; -1 nothing more for now; -2 TLS wants to write; -3 failed. */
static long bm_fc_recv(bm_fc *c, char *buf, size_t n) {
    if (c->tls) return bm_tls_impl->read(c->tls, buf, n);
    for (;;) {
        ssize_t r = read(c->fd, buf, n);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : -3;
    }
}

static bool bm_fc_open(bm_fc *c) {
    for (; c->ai < c->addrs.n; c->ai++) {
        struct sockaddr_storage *sa = &c->addrs.a[c->ai];
        if (sa->ss_family == AF_INET) ((struct sockaddr_in *)sa)->sin_port = htons((uint16_t)c->port);
        else if (sa->ss_family == AF_INET6) ((struct sockaddr_in6 *)sa)->sin6_port = htons((uint16_t)c->port);
        int fd = socket(sa->ss_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        c->fd = fd;
        c->registered = c->reading = c->writing = false;
        int rc = connect(fd, (struct sockaddr *)sa, c->addrs.len[c->ai]);
        if (rc == 0) { c->connecting = false; return true; }
        if (errno == EINPROGRESS || errno == EINTR) {
            c->connecting = true;
            bm_fc_interest(c, false, true);
            return true;
        }
        c->err = errno;
        close(fd);
        c->fd = -1;
    }
    return false;
}

static void bm_fr_connect_failed(bm_fr *r, int err) {
    if (r->unix_path.p->len) { bm_fr_fail(r, "FailedToOpenSocket", "Was there a typo in the url or port?"); return; }
    if (err == ECONNREFUSED) bm_fr_fail(r, "ConnectionRefused", "Unable to connect. Is the computer able to access the url?");
    else if (err == ETIMEDOUT) bm_fr_fail(r, "ConnectionTimeout", "Unable to connect. Is the computer able to access the url?");
    else bm_fr_fail(r, "ConnectionRefused", "Unable to connect. Is the computer able to access the url?");
}

static void bm_fr_send(bm_fr *r, bm_fc *c) {
    r->c = c;
    c->r = r;
    r->sent = 0;
    if (c->tls && !r->merged && r->body.p->len && r->body.p->len <= 8192) {
        /* one TLS record (and one write) for a small request */
        bm_sb_push(&r->head, r->body.p->data, (size_t)r->body.p->len);
        r->merged = true;
    }
    r->state = BM_FR_HEAD;
    r->got_any = false;
    if (!bm_fc_write(c)) {
        /* a pooled connection the server closed: try once more on a new one */
        bool retry = c->reused && !r->retried;
        c->r = NULL;
        r->c = NULL;
        bm_fc_close(c);
        if (retry) { r->retried = true; bm_fr_step(r); }
        else bm_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", BM_FETCH_VERBOSE);
    }
}

/* Continues a TLS handshake; sends the request once it's done. */
static void bm_fc_handshake(bm_fc *c) {
    bm_fr *r = c->r;
    int rc = bm_tls_impl->handshake(c->tls);
    if (rc == 0) { c->handshaking = false; bm_fr_send(r, c); return; }
    /* still connecting (the ClientHello went early): wait for the connection, and report a
     * refused one as such, not as a TLS error */
    if (c->connecting) {
        /* the write took the connect's error (a loopback refusal is instant on Linux): keep it,
         * as SO_ERROR won't say it again */
        if (rc < 0) c->early_err = errno ? errno : ECONNREFUSED;
        bm_fc_interest(c, true, true);
        return;
    }
    if (rc == 1) { bm_fc_interest(c, true, false); return; }
    if (rc == 2) { bm_fc_interest(c, true, true); return; }
    const char *code = "ERR_SSL";
    char msg[512];
    bm_tls_impl->why(c->tls, r->url.p->data, &code, msg, sizeof msg);
    bm_fr_fail(r, code, "%s", msg);
}

static void bm_fc_start_tls(bm_fc *c);
static void bm_fr_complete(bm_fr *r);

/* Sends what's left of the CONNECT request; true once it's all out. */
static bool bm_fc_send_ctl(bm_fc *c) {
    while (c->ctl_off < c->ctl.len) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(c->fd, c->ctl.data + c->ctl_off, c->ctl.len - c->ctl_off, MSG_NOSIGNAL);
#else
        ssize_t w = send(c->fd, c->ctl.data + c->ctl_off, c->ctl.len - c->ctl_off, 0);
#endif
        if (w > 0) { c->ctl_off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { bm_fc_interest(c, true, true); return false; }
        return false;
    }
    bm_fc_interest(c, true, false);
    return true;
}

/* The proxy's answer to CONNECT: 200 starts TLS through the tunnel; anything else is the
 * response (a 407, say), as Bun gives it. */
static void bm_fc_tunnel(bm_fc *c, bool readable, bool writable) {
    bm_fr *r = c->r;
    if (writable && c->ctl_off < c->ctl.len && !bm_fc_send_ctl(c) && c->ctl_off < c->ctl.len) return;
    if (!readable) return;
    for (;;) {
        if (c->in_cap - c->in_len < 4096) {
            size_t cap = c->in_cap ? c->in_cap * 2 : 16384;
            c->in = bm_realloc(c->in, cap);
            c->in_cap = cap;
        }
        ssize_t n = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
        if (n > 0) { c->in_len += (size_t)n; continue; }
        if (n == 0) { bm_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", BM_FETCH_VERBOSE); return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        bm_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", BM_FETCH_VERBOSE);
        return;
    }
    const char *e = NULL;
    for (size_t i = 3; i < c->in_len; i++) if (memcmp(c->in + i - 3, "\r\n\r\n", 4) == 0) { e = c->in + i + 1; break; }
    if (!e) return; /* more to come */
    c->tunneling = false;
    if (c->in_len >= 12 && memcmp(c->in, "HTTP/1.", 7) == 0 && memcmp(c->in + 9, "200", 3) == 0) {
        c->in_off = c->in_len = 0; /* the tunnel is open */
        bm_fc_start_tls(c);
        return;
    }
    /* refused: parse the proxy's response as the answer */
    c->no_reuse = true;
    r->https = false;
    r->state = BM_FR_HEAD;
    r->got_any = true;
    int st = bm_fr_parse(r, c);
    if (r->result != 1) return;
    if (st < 0) { bm_fr_fail(r, "Malformed_HTTP_Response", "Malformed_HTTP_Response fetching \"%s\". %s", r->url.p->data, BM_FETCH_VERBOSE); return; }
    r->keep = false;
    if (st > 0) {
        r->c = NULL;
        bm_fc_release(c, false);
        bm_fr_complete(r);
    }
}

/* The TCP connection is up: tunnel through the proxy (https), start TLS, or send the request. */
static void bm_fc_connected(bm_fc *c) {
    bm_fr *r = c->r;
    if (!r->https) { bm_fr_send(r, c); return; }
    if (c->tls) { bm_fc_handshake(c); return; } /* the ClientHello, made while connecting, goes out */
    if (r->via_proxy) {
        bm_sb *h = &c->ctl;
        h->len = 0;
        c->ctl_off = 0;
        char line[600];
        int n = snprintf(line, sizeof line, "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n", r->host, r->port, r->host, r->port);
        bm_sb_push(h, line, (size_t)n);
        if (r->proxy_auth) {
            bm_sb_push(h, "Proxy-Authorization: ", 21);
            bm_sb_push(h, r->proxy_auth, strlen(r->proxy_auth));
            bm_sb_push(h, "\r\n", 2);
        }
        bm_sb_push(h, "\r\n", 2);
        c->tunneling = true;
        bm_fc_send_ctl(c);
        return;
    }
    bm_fc_start_tls(c);
}

static void bm_fc_start_tls(bm_fc *c) {
    bm_fr *r = c->r;
    c->tls = bm_tls_impl->open(c->fd, r->host, r->key, !r->insecure, r->ca.p->data, (size_t)r->ca.p->len);
    if (!c->tls) { bm_fr_fail(r, "ERR_SSL", "TLS setup failed"); return; }
    c->handshaking = true;
    bm_fc_handshake(c);
}

/* Connects to the resolved addresses (DNS done). */
static void bm_fr_connect(bm_fr *r, const bm_addrs *addrs) {
    bm_fc *c = bm_alloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->io.ready = bm_fc_ready;
    c->fd = -1;
    c->addrs = *addrs;
    c->port = r->dial_port;
    c->origin = bm_origin_get(r->key);
    c->err = ECONNREFUSED;
    if (!bm_fc_open(c)) {
        int err = c->err;
        bm_free(c);
        bm_fr_connect_failed(r, err);
        return;
    }
    r->c = c;
    c->r = r;
    if (!c->connecting) { bm_fc_connected(c); return; }
    /* make the ClientHello (the key shares are the costly part) while TCP connects: it waits
     * in the TLS write buffer until the socket is writable */
    if (r->https && !r->via_proxy) bm_fc_start_tls(c);
}

static void bm_dns_resolved(bm_dns *d) {
    bm_fr *w = d->waiters;
    d->waiters = NULL;
    while (w) {
        bm_fr *next = w->dns_next;
        w->dns_next = NULL;
        if (w->result == 1) {
            if (d->err) bm_fr_fail(w, "ENOTFOUND", "getaddrinfo ENOTFOUND %s", w->dial_host);
            else bm_fr_connect(w, &d->addrs);
        }
        w = next;
    }
}

static void bm_dns_ready(bm_io *h, bool readable, bool writable, bool broken) {
    (void)h; (void)writable; (void)broken;
    if (!readable) return;
    char buf[64];
    while (read(bm_dns_pipe[0], buf, sizeof buf) > 0) {}
    pthread_mutex_lock(&bm_dns_mu);
    bm_dns *done = bm_dns_done;
    bm_dns_done = NULL;
    pthread_mutex_unlock(&bm_dns_mu);
    for (bm_dns *d = done, *next; d; d = next) {
        next = d->job_next;
        d->resolving = false;
        d->expires = bm_loop_update() + (d->err ? 1000 : BM_DNS_TTL_MS);
        bm_io_refs--;
        bm_dns_resolved(d);
    }
}

static bool bm_numeric_host(const char *host, bm_addrs *out) {
    out->n = 0;
    struct sockaddr_in *v4 = (struct sockaddr_in *)&out->a[0];
    memset(v4, 0, sizeof *v4);
    if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        out->len[0] = sizeof *v4;
        out->n = 1;
        return true;
    }
    size_t hl = strlen(host);
    if (hl >= 2 && host[0] == '[' && host[hl - 1] == ']' && hl < 64) {
        char bare[64];
        memcpy(bare, host + 1, hl - 2);
        bare[hl - 2] = 0;
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&out->a[0];
        memset(v6, 0, sizeof *v6);
        if (inet_pton(AF_INET6, bare, &v6->sin6_addr) == 1) {
            v6->sin6_family = AF_INET6;
            out->len[0] = sizeof *v6;
            out->n = 1;
            return true;
        }
    }
    if (strcmp(host, "localhost") == 0) {
        memset(&out->a[0], 0, sizeof out->a[0]);
        memset(&out->a[1], 0, sizeof out->a[1]);
        struct sockaddr_in *a = (struct sockaddr_in *)&out->a[0];
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        out->len[0] = sizeof *a;
        struct sockaddr_in6 *b = (struct sockaddr_in6 *)&out->a[1];
        b->sin6_family = AF_INET6;
        b->sin6_addr = in6addr_loopback;
        out->len[1] = sizeof *b;
        out->n = 2;
        return true;
    }
    return false;
}

/* Sends r on a pooled connection, or looks up its host and connects. */
static void bm_fr_step(bm_fr *r) {
    bm_origin *o = bm_origin_get(r->key);
    while (o->idle) {
        bm_fc *c = o->idle;
        o->idle = c->next;
        o->nidle--;
        if (c->dead) continue;
        bm_fr_send(r, c);
        return;
    }
    bm_addrs addrs;
    if (r->unix_path.p->len) {
        /* Bun's `unix`: the socket, whatever the URL's host */
        struct sockaddr_un *un = (struct sockaddr_un *)&addrs.a[0];
        memset(un, 0, sizeof *un);
        un->sun_family = AF_UNIX;
        if ((size_t)r->unix_path.p->len >= sizeof un->sun_path) { bm_fr_fail(r, "ENAMETOOLONG", "The socket path is too long: %s", r->unix_path.p->data); return; }
        memcpy(un->sun_path, r->unix_path.p->data, (size_t)r->unix_path.p->len);
        addrs.len[0] = (socklen_t)sizeof *un;
        addrs.n = 1;
        bm_fr_connect(r, &addrs);
        return;
    }
    if (bm_numeric_host(r->dial_host, &addrs)) { bm_fr_connect(r, &addrs); return; }
    uint32_t b = bm_hash_cstr(r->dial_host) % BM_DNS_BUCKETS;
    bm_dns *d = bm_dns_cache[b];
    while (d && strcmp(d->host, r->dial_host) != 0) d = d->next;
    if (d && !d->resolving && d->expires > bm_loop_update()) {
        if (d->err) bm_fr_fail(r, "ENOTFOUND", "getaddrinfo ENOTFOUND %s", r->dial_host);
        else bm_fr_connect(r, &d->addrs);
        return;
    }
    if (!d) {
        d = bm_alloc(sizeof *d);
        memset(d, 0, sizeof *d);
        size_t hl = strlen(r->dial_host);
        d->host = bm_alloc(hl + 1);
        memcpy(d->host, r->dial_host, hl + 1);
        d->next = bm_dns_cache[b];
        bm_dns_cache[b] = d;
    }
    r->dns_next = d->waiters;
    d->waiters = r;
    if (!d->resolving) {
        d->resolving = true;
        bm_dns_queue(d);
    }
}

static void bm_fr_reset_response(bm_fr *r) {
    r->state = BM_FR_HEAD;
    r->status = 0;
    r->rbody.len = 0;
    if (r->dec) { bm_codec->close(r->dec); r->dec = NULL; }
    r->raw.len = r->raw_off = 0;
    r->draining = false;
    r->discard = false;
    r->utf8_pos = 0;
    r->utf8_bad = false;
}

/* The whole response is in: follow a redirect, or decode the body and settle. */
static void bm_fr_complete(bm_fr *r) {
    int s = r->status;
    if (r->redirect_mode != BM_FETCH_MANUAL && bm_redirect_status(s) && r->location.len > 0) {
        if (r->redirect_mode == BM_FETCH_ERROR) {
            bm_fr_fail(r, "UnexpectedRedirect", "UnexpectedRedirect fetching \"%s\". %s", r->url.p->data, BM_FETCH_VERBOSE);
            return;
        }
        if (++r->redirects > 20) {
            bm_fr_fail(r, "TooManyRedirects", "The response redirected too many times. %s", BM_FETCH_VERBOSE);
            return;
        }
        bm_str loc = bm_str_from(r->location.data, r->location.len);
        bm_str next = bm_native_urlNormalize(loc, r->url);
        bm_str_release(loc);
        if (next.p->len == 0) {
            bm_fr_fail(r, "InvalidRedirectURL", "The redirect URL \"%.*s\" is invalid", (int)r->location.len, r->location.data);
            return;
        }
        const char *m = r->method.p->data;
        if ((s == 303 && strcmp(m, "GET") != 0 && strcmp(m, "HEAD") != 0) || ((s == 301 || s == 302) && strcmp(m, "POST") == 0)) {
            bm_str_release(r->method);
            r->method = bm_str_from("GET", 3);
            bm_str_release(r->body);
            r->body = BM_EMPTY_STR;
            static const char *const body_headers[] = {"content-type", "content-length", "content-encoding", "content-language", "content-location", "transfer-encoding"};
            for (size_t i = 0; i < sizeof body_headers / sizeof *body_headers; i++) {
                bm_str name = bm_str_from(body_headers[i], strlen(body_headers[i]));
                bm_str h2 = bm_native_headerRemove(r->headers, name);
                bm_str_release(r->headers);
                bm_str_release(name);
                r->headers = h2;
            }
        }
        /* another origin doesn't get the credentials */
        char *old_key = r->key;
        r->key = NULL;
        bm_str_release(r->url);
        r->url = next;
        if (!bm_fr_prepare(r)) { bm_free(old_key); return; }
        if (strcmp(old_key, r->key) != 0) {
            static const char *const creds[] = {"authorization", "proxy-authorization", "cookie"};
            bool removed = false;
            for (size_t i = 0; i < 3; i++) {
                if (!bm_hdr_has(r->headers, creds[i])) continue;
                bm_str name = bm_str_from(creds[i], strlen(creds[i]));
                bm_str h2 = bm_native_headerRemove(r->headers, name);
                bm_str_release(r->headers);
                bm_str_release(name);
                r->headers = h2;
                removed = true;
            }
            if (removed) bm_fr_prepare(r);
        }
        bm_free(old_key);
        r->redirected = true;
        r->retried = false;
        bm_fr_reset_response(r);
        bm_fr_step(r);
        return;
    }
    if (bm_fr_decoding(r) && (r->dec || r->raw.len)) {
        /* the rest of the body (checked as UTF-8 as it's decoded) */
        if (r->whole) {
            if (!bm_fr_decode_whole(r)) return;
        } else {
            if (r->streaming && !bm_fr_decode(r, false)) return;
            if (!r->streaming || r->raw_off < r->raw.len) {
                r->draining = true; /* decoded as it's read */
                if (r->read_p && r->rbody.len) bm_fr_resolve(&r->read_p, (bm_int)r->rbody.len);
                return;
            }
            if (!bm_fr_decode(r, true)) return;
        }
    } else if (r->enc && !bm_fr_decoding(r)) {
        r->utf8_bad = true; /* not decoded (decompress: false): text() checks it */
    }
    bm_fr_settle(r, 0);
}

static void bm_fc_ready(bm_io *h, bool readable, bool writable, bool broken) {
    bm_fc *c = (bm_fc *)h;
    if (c->dead) return;
    bm_fr *r = c->r;
    if (!r) {
        /* idle: the server closed it (or sent something unasked for) */
        if (readable || broken) {
            if (c->tls && !broken) {
                /* a late TLS 1.3 session ticket arrives this way: that's all right */
                char b;
                if (bm_tls_impl->read(c->tls, &b, 1) == -1) return;
            }
            bm_origin_unlink(c);
            bm_fc_close(c);
        }
        return;
    }
    if (c->connecting) {
        if (!writable && !readable && !broken) return;
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0) err = errno;
        if (!err) err = c->early_err;
        if (!err && c->ai > 0) {
            /* an event left over from an address given up on: this one may still be connecting */
            struct sockaddr_storage peer;
            socklen_t pl = sizeof peer;
            if (getpeername(c->fd, (struct sockaddr *)&peer, &pl) != 0) {
                if (errno == ENOTCONN) return;
                err = errno;
            }
        }
        if (err) {
            if (c->tls) { bm_tls_impl->close(c->tls, false); c->tls = NULL; c->handshaking = false; c->early_err = 0; }
            close(c->fd);
            c->fd = -1;
            c->ai++;
            if (bm_fc_open(c)) {
                if (!c->connecting) bm_fc_connected(c);
                return;
            }
            c->r = NULL;
            r->c = NULL;
            bm_fc_close(c);
            bm_fr_connect_failed(r, c->err ? c->err : err);
            return;
        }
        c->connecting = false;
        bm_fc_connected(c);
        return;
    }
    if (c->tunneling) {
        if (broken) { bm_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", BM_FETCH_VERBOSE); return; }
        bm_fc_tunnel(c, readable, writable);
        return;
    }
    if (c->handshaking) {
        if (readable || writable || broken) bm_fc_handshake(c);
        return;
    }
    if (writable && r->sent < bm_fr_out_len(r)) {
        if (!bm_fc_write(c)) {
            bool retry = c->reused && !r->retried && !r->got_any;
            c->r = NULL;
            r->c = NULL;
            bm_fc_close(c);
            if (retry) { r->retried = true; bm_fr_step(r); }
            else bm_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", BM_FETCH_VERBOSE);
            return;
        }
    }
    if (!readable && !broken) return;
    bool eof = false, error = false;
    for (;;) {
        /* a known-length body goes straight into its buffer */
        if (r->state == BM_FR_FIXED && c->in_off == c->in_len && !r->discard) {
            size_t want = (size_t)(r->remaining < (1 << 20) ? r->remaining : (1 << 20));
            bm_sb *sink = bm_fr_sink(r);
            if (sink->cap - sink->len < want) bm_sb_grow(sink, sink->len + want);
            long n = bm_fc_recv(c, sink->data + sink->len, want);
            if (n > 0) {
                r->got_any = true;
                sink->len += (size_t)n;
                r->remaining -= n;
                if (!bm_fr_decoding(r)) bm_fr_check_utf8(r);
                bool full = bm_fr_data(r);
                if (r->result != 1) return; /* the body couldn't be decoded */
                if (r->remaining == 0) { r->state = BM_FR_DONE; break; }
                if (full) { r->paused = true; bm_fc_interest(c, false, false); break; }
                /* TLS returns a record at a time: read until it has no more */
                if ((size_t)n == want || c->tls) continue;
                break;
            }
            if (n == 0) { eof = true; break; }
            if (n == -2) { bm_fc_interest(c, true, true); break; }
            if (n == -3) error = true;
            break;
        }
        if (c->in_off == c->in_len) c->in_off = c->in_len = 0;
        if (c->in_cap - c->in_len < 16384) {
            if (c->in_off) {
                memmove(c->in, c->in + c->in_off, c->in_len - c->in_off);
                c->in_len -= c->in_off;
                c->in_off = 0;
            }
            if (c->in_cap - c->in_len < 16384) {
                size_t cap = c->in_cap ? c->in_cap * 2 : 65536;
                c->in = bm_realloc(c->in, cap);
                c->in_cap = cap;
            }
        }
        size_t room = c->in_cap - c->in_len;
        long n = bm_fc_recv(c, c->in + c->in_len, room);
        if (n > 0) {
            r->got_any = true;
            c->in_len += (size_t)n;
            int st = bm_fr_parse(r, c);
            if (r->result != 1) return;
            if (st < 0) {
                bm_fr_fail(r, "Malformed_HTTP_Response", "Malformed_HTTP_Response fetching \"%s\". %s", r->url.p->data, BM_FETCH_VERBOSE);
                return;
            }
            if (st > 0) break;
            if (r->streaming && (r->rbody.len >= BM_STREAM_HIGH || r->raw_off < r->raw.len)) { r->paused = true; bm_fc_interest(c, false, false); break; }
            if ((size_t)n == room || c->tls) continue;
            break;
        }
        if (n == 0) { eof = true; break; }
        if (n == -2) { bm_fc_interest(c, true, true); break; }
        if (n == -3) error = true;
        break;
    }
    if (r->state == BM_FR_DONE) {
        bool leftover = c->in_off < c->in_len;
        r->c = NULL;
        bm_fc_release(c, r->keep && !leftover && !eof && !error);
        bm_fr_complete(r);
        return;
    }
    if (eof || error) {
        if (r->state == BM_FR_UNTIL_CLOSE && eof) {
            r->state = BM_FR_DONE;
            r->c = NULL;
            bm_fc_release(c, false);
            bm_fr_complete(r);
            return;
        }
        bool retry = c->reused && !r->retried && !r->got_any;
        c->r = NULL;
        r->c = NULL;
        bm_fc_close(c);
        if (retry) { r->retried = true; bm_fr_reset_response(r); bm_fr_step(r); return; }
        bm_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", BM_FETCH_VERBOSE);
    }
}

/* ---- after fork (a server worker): the parent's connections, lookups and threads aren't ours */

static void bm_fetch_after_fork(void) {
    pthread_mutex_init(&bm_dns_mu, NULL);
    pthread_cond_init(&bm_dns_cv, NULL);
    bm_dns_threads = bm_dns_idle = 0;
    bm_dns_jobs = bm_dns_done = NULL;
    if (bm_dns_pipe[0] >= 0) { close(bm_dns_pipe[0]); close(bm_dns_pipe[1]); bm_dns_pipe[0] = bm_dns_pipe[1] = -1; }
    for (int b = 0; b < BM_DNS_BUCKETS; b++) bm_dns_cache[b] = NULL;
    for (int b = 0; b < BM_ORIGIN_BUCKETS; b++) {
        for (bm_origin *o = bm_origins[b]; o; o = o->next) {
            for (bm_fc *c = o->idle; c; c = c->next) if (c->fd >= 0) close(c->fd);
            o->idle = NULL;
            o->nidle = 0;
        }
    }
    for (bm_int i = 0; i < bm_fr_cap; i++) {
        bm_fr *r = bm_fr_table[i];
        if (r && r->result == 1) {
            if (r->c && r->c->fd >= 0) { close(r->c->fd); r->c->fd = -1; }
            r->c = NULL;
            r->result = -1;
            r->code = "ECONNRESET";
        }
    }
    bm_io_refs = 0;
}

/* ---- natives */

static bm_fr *bm_fr_get(bm_int id) {
    bm_int i = (id & 0xffffffff) - 1;
    if (i < 0 || i >= bm_fr_cap) return NULL;
    bm_fr *r = bm_fr_table[i];
    return r && r->id == id ? r : NULL;
}

bm_int bm_native_fetchStart(bm_str method, bm_str url, bm_str headers, bm_str body, bm_int redirect, bm_int flags, bm_str ca, bm_str unix_path, bm_str proxy) {
    bm_io_after_batch = bm_fc_reap;
    bm_io_after_fork = bm_fetch_after_fork;
    signal(SIGPIPE, SIG_IGN);
    if (bm_fr_nfree == 0) {
        bm_int cap = bm_fr_cap ? bm_fr_cap * 2 : 16;
        bm_fr_table = bm_realloc(bm_fr_table, (size_t)cap * sizeof *bm_fr_table);
        bm_fr_free = bm_realloc(bm_fr_free, (size_t)cap * sizeof *bm_fr_free);
        for (bm_int i = cap - 1; i >= bm_fr_cap; i--) { bm_fr_table[i] = NULL; bm_fr_free[bm_fr_nfree++] = i; }
        bm_fr_cap = cap;
    }
    bm_int slot = bm_fr_free[--bm_fr_nfree];
    bm_fr *r = bm_alloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->id = (bm_int)(((uint64_t)++bm_fr_gen << 32) | (uint64_t)(slot + 1));
    bm_fr_table[slot] = r;
    r->result = 1;
    r->head_state = 1;
    bm_io_refs++;
    bm_str_retain(method);
    bm_str_retain(headers);
    bm_str_retain(body);
    r->method = method;
    r->headers = headers;
    r->body = body;
    r->redirect_mode = (int)redirect;
    r->decompress = (flags & BM_FETCH_DECOMPRESS) != 0;
    r->insecure = (flags & BM_FETCH_INSECURE) != 0;
    bm_str_retain(ca);
    r->ca = ca;
    bm_str_retain(unix_path);
    r->unix_path = unix_path;
    bm_str_retain(proxy);
    r->proxy = proxy;
    r->url = bm_native_urlNormalize(url, BM_EMPTY_STR);
    if (r->url.p->len == 0) {
        bm_fr_fail(r, "ERR_INVALID_URL", "fetch() URL is invalid");
        return r->id;
    }
    /* the fragment isn't sent, and isn't part of the response URL */
    const char *hash = memchr(r->url.p->data, '#', (size_t)r->url.p->len);
    if (hash) {
        bm_str cut = bm_str_from(r->url.p->data, (size_t)(hash - r->url.p->data));
        bm_str_release(r->url);
        r->url = cut;
    }
    if (bm_fr_prepare(r)) bm_fr_step(r);
    return r->id;
}

/* A Promise<int>: settled with `now` unless `pending`, else kept in *slot until it settles. */
static bm_promise *bm_fr_promise(bm_promise **slot, bool pending, bm_int now) {
    bm_promise *p = bm_promise_new(&bm_type_int);
    if (!pending || !slot) {
        bm_promise_resolve(p, &now);
        return p;
    }
    if (*slot) bm_promise_release(*slot); /* a reader asking again: the new promise replaces it */
    bm_promise_retain(p);
    *slot = p;
    return p;
}

/* A body that has all arrived is still being decoded: decode more (all of it for a whole-body
 * read), and settle once it's done. */
static void bm_fr_drain(bm_fr *r, bool all) {
    if (!r->draining || r->result != 1) return;
    if (all) {
        if (!bm_fr_decode_whole(r)) return;
    } else {
        if (!bm_fr_decode(r, false)) return;
        if (r->raw_off < r->raw.len) return;
        if (!bm_fr_decode(r, true)) return; /* complete? */
    }
    r->draining = false;
    bm_fr_settle(r, 0);
}

/* The response head arrived (0), or the request failed (< 0). */
bm_promise *bm_native_fetchWait(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    return bm_fr_promise(r ? &r->done : NULL, r && r->head_state == 1, r ? r->head_state : -1);
}

/* The whole body is in (0), or it failed (< 0). */
bm_promise *bm_native_fetchBodyWait(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    if (r && !r->whole) {
        /* read whole: decoded at once when it's all in (now, if it is) */
        r->whole = true;
        bm_fr_drain(r, true);
    }
    return bm_fr_promise(r ? &r->body_p : NULL, r && r->result == 1, r ? r->result : -1);
}

/* Streaming (res.body): bytes to take (> 0), the end (0), or a failure (< 0). */
bm_promise *bm_native_fetchRead(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    if (!r) return bm_fr_promise(NULL, false, -1);
    r->streaming = true;
    /* what arrived before reading began */
    if (!r->rbody.len && r->draining) bm_fr_drain(r, false);
    else if (!r->rbody.len && r->raw_off < r->raw.len && r->result == 1) bm_fr_decode(r, false);
    if (r->rbody.len) return bm_fr_promise(NULL, false, (bm_int)r->rbody.len);
    return bm_fr_promise(&r->read_p, r->result == 1, r->result);
}

/* The body bytes that have arrived (and haven't been taken). */
bm_arr bm_native_fetchTake(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    if (!r) return BM_EMPTY_ARR;
    bm_arr a = bm_arr_with_capacity(&bm_type_u8, (bm_int)r->rbody.len);
    if (r->rbody.len) memcpy(bm_arr_data(a), r->rbody.data, r->rbody.len);
    a.len = (bm_int)r->rbody.len;
    r->rbody.len = 0;
    /* decode what waited for room (while it's still more than the reader wants, stay paused) */
    if (r->draining) { bm_fr_drain(r, false); return a; }
    if (r->raw_off < r->raw.len && r->result == 1 && (!bm_fr_decode(r, false) || r->raw_off < r->raw.len)) return a;
    if (r->paused && r->c) {
        /* the reader caught up: read on (what's buffered in TLS or unparsed won't raise an event) */
        r->paused = false;
        bm_fc *c = r->c;
        bm_fc_interest(c, true, false);
        bm_fc_ready(&c->io, true, false, false);
    }
    return a;
}

/* Response.clone(): one more reader will take the whole body. */
void bm_native_fetchShare(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    if (r) r->shares++;
}

static void bm_fetch_handle_release(void *p) { bm_native_fetchFree(*(bm_int *)p); }
static const bm_type bm_type_fetch_handle = { sizeof(bm_int), NULL, bm_fetch_handle_release, NULL, NULL, NULL, NULL };

/* A Response's hold on request `id`: a settled promise whose release (the Response going away)
 * frees the request, and aborts it if the body is still arriving. */
bm_promise *bm_native_fetchHandle(bm_int id) {
    bm_promise *p = bm_promise_new(&bm_type_fetch_handle);
    bm_promise_resolve(p, &id);
    return p;
}

bm_int bm_native_fetchStatus(bm_int id) { bm_fr *r = bm_fr_get(id); return r ? r->status : 0; }
bm_str bm_native_fetchStatusText(bm_int id) { bm_fr *r = bm_fr_get(id); return r ? bm_str_from(r->status_text.data, r->status_text.len) : BM_EMPTY_STR; }
bm_str bm_native_fetchHeaders(bm_int id) { bm_fr *r = bm_fr_get(id); return r ? bm_str_from(r->rheaders.data, r->rheaders.len) : BM_EMPTY_STR; }
bm_str bm_native_fetchUrl(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    if (!r) return BM_EMPTY_STR;
    bm_str_retain(r->url);
    return r->url;
}
bool bm_native_fetchRedirected(bm_int id) { bm_fr *r = bm_fr_get(id); return r && r->redirected; }
/* The body is valid UTF-8 without a BOM: text() can return it as it is. */
bool bm_native_fetchBodyClean(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    if (!r || r->utf8_bad || r->utf8_pos != r->rbody.len) return false;
    const uint8_t *p = (const uint8_t *)r->rbody.data;
    return !(r->rbody.len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF);
}

bm_str bm_native_fetchBody(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    if (!r) return BM_EMPTY_STR;
    if (r->shares > 0) { /* a clone reads it too */
        r->shares--;
        return bm_str_from(r->rbody.data, r->rbody.len);
    }
    return bm_str_from_sb(&r->rbody);
}
bm_str bm_native_fetchErrorCode(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    return r && r->code ? bm_str_from(r->code, strlen(r->code)) : BM_EMPTY_STR;
}
bm_str bm_native_fetchErrorMessage(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    return r ? bm_str_from(r->message.data, r->message.len) : BM_EMPTY_STR;
}

/* Abandons a request in flight (AbortSignal): its connection is closed, and fetchWait's promise
 * settles with -2. */
void bm_native_fetchAbort(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    if (!r || r->result != 1) return;
    if (r->c) {
        bm_fc *c = r->c;
        r->c = NULL;
        c->r = NULL;
        bm_fc_close(c);
    }
    /* a lookup keeps going (others may want it); this request stops waiting for it */
    for (int b = 0; b < BM_DNS_BUCKETS; b++)
        for (bm_dns *d = bm_dns_cache[b]; d; d = d->next)
            for (bm_fr **pp = &d->waiters; *pp; pp = &(*pp)->dns_next)
                if (*pp == r) { *pp = r->dns_next; goto unlinked; }
unlinked:
    r->code = "AbortError";
    bm_fr_settle(r, -2);
}

void bm_native_fetchFree(bm_int id) {
    bm_fr *r = bm_fr_get(id);
    if (!r) return;
    if (r->result == 1) bm_native_fetchAbort(id);
    bm_int slot = (id & 0xffffffff) - 1;
    bm_fr_table[slot] = NULL;
    bm_fr_free[bm_fr_nfree++] = slot;
    if (r->done) bm_promise_release(r->done);
    if (r->body_p) bm_promise_release(r->body_p);
    if (r->read_p) bm_promise_release(r->read_p);
    bm_str_release(r->method);
    bm_str_release(r->headers);
    bm_str_release(r->body);
    bm_str_release(r->url);
    bm_str_release(r->ca);
    bm_str_release(r->unix_path);
    bm_str_release(r->proxy);
    bm_free(r->dial_host);
    bm_free(r->proxy_auth);
    bm_free(r->host);
    bm_free(r->key);
    bm_sb_free(&r->head);
    bm_sb_free(&r->status_text);
    bm_sb_free(&r->rheaders);
    bm_sb_free(&r->rbody);
    if (r->dec) bm_codec->close(r->dec);
    bm_sb_free(&r->raw);
    bm_sb_free(&r->location);
    bm_sb_free(&r->message);
    bm_free(r);
}

/* ---- bodies: bytes <-> strings, and text() decoding */

bm_str bm_native_bytesToString(bm_arr bytes) {
    return bm_str_from((const char *)bm_arr_data(bytes), (size_t)bytes.len);
}

bm_arr bm_native_stringToBytes(bm_str s) {
    bm_arr a = bm_arr_with_capacity(&bm_type_u8, s.p->len);
    if (s.p->len) memcpy(bm_arr_data(a), s.p->data, (size_t)s.p->len);
    a.len = s.p->len;
    return a;
}

/* WHATWG "UTF-8 decode": drops a leading BOM and replaces each maximal invalid subsequence
 * with U+FFFD. Returns s itself when it is already clean. */
bm_str bm_native_utf8Clean(bm_str s) {
    const uint8_t *p = (const uint8_t *)s.p->data;
    size_t n = (size_t)s.p->len, i = 0;
    bool bom = n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF;
    if (bom) i = 3;
    /* validate first: most bodies are fine */
    size_t j = i;
    bool valid = bm_utf8_scan(p, n, &j) && j == n;
    if (valid) {
        if (!bom) { bm_str_retain(s); return s; }
        return bm_str_from((const char *)p + 3, n - 3);
    }
    bm_sb out = {0};
    bm_sb_grow(&out, n + 16);
    bm_sb_push(&out, (const char *)p + i, j - i);
    static const char fffd[3] = {(char)0xEF, (char)0xBF, (char)0xBD};
    i = j;
    while (i < n) {
        uint8_t b = p[i];
        if (b < 0x80) { bm_sb_push_char(&out, (char)b); i++; continue; }
        size_t need = b >= 0xC2 && b <= 0xDF ? 1 : b >= 0xE0 && b <= 0xEF ? 2 : b >= 0xF0 && b <= 0xF4 ? 3 : 0;
        if (!need) { bm_sb_push(&out, fffd, 3); i++; continue; }
        uint8_t lo = b == 0xE0 ? 0xA0 : b == 0xF0 ? 0x90 : 0x80, hi = b == 0xED ? 0x9F : b == 0xF4 ? 0x8F : 0xBF;
        size_t k = 1;
        while (k <= need && i + k < n) {
            uint8_t c = p[i + k];
            if (c < lo || c > hi) break;
            lo = 0x80;
            hi = 0xBF;
            k++;
        }
        if (k == need + 1) { bm_sb_push(&out, (const char *)p + i, k); i += k; }
        else { bm_sb_push(&out, fffd, 3); i += k; } /* the maximal subpart becomes one U+FFFD */
    }
    return bm_str_from_sb(&out);
}

/* ---- Headers: combined values and sorted entries, as the Fetch standard defines them */

/* Every `name` line's value, joined with ", " ("" if none). */
bm_str bm_native_headerGet(bm_str block, bm_str name) {
    const char *s = block.p->data, *end = s + block.p->len;
    size_t n = (size_t)name.p->len;
    bm_sb out = {0};
    int found = 0;
    const char *first = NULL;
    size_t first_len = 0;
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *le = nl ? nl : end;
        if ((size_t)(le - s) > n && strncasecmp(s, name.p->data, n) == 0 && s[n] == ':') {
            const char *v = s + n + 1, *ve = le;
            if (ve > v && ve[-1] == '\r') ve--;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            if (found == 0) { first = v; first_len = (size_t)(ve - v); }
            else {
                if (found == 1) bm_sb_push(&out, first, first_len);
                bm_sb_push(&out, ", ", 2);
                bm_sb_push(&out, v, (size_t)(ve - v));
            }
            found++;
        }
        if (!nl) break;
        s = nl + 1;
    }
    if (found == 1) return bm_str_from(first, first_len);
    return bm_str_from_sb(&out);
}

typedef struct { const char *name, *value; size_t nlen, vlen, order; } bm_hdr_line;

static int bm_hdr_cmp(const void *a, const void *b) {
    const bm_hdr_line *x = a, *y = b;
    size_t n = x->nlen < y->nlen ? x->nlen : y->nlen;
    int c = strncasecmp(x->name, y->name, n);
    if (c) return c;
    if (x->nlen != y->nlen) return x->nlen < y->nlen ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* [name, value, name, value, ...]: names lower-cased and sorted, repeated names combined
 * (except set-cookie, one entry per cookie). */
bm_arr bm_native_headerEntries(bm_str block) {
    const char *s = block.p->data, *end = s + block.p->len;
    size_t cap = 16, n = 0;
    bm_hdr_line *lines = bm_alloc(cap * sizeof *lines);
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *le = nl ? nl : end;
        const char *colon = memchr(s, ':', (size_t)(le - s));
        if (colon && colon > s) {
            const char *v = colon + 1, *ve = le;
            if (ve > v && ve[-1] == '\r') ve--;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            if (n == cap) { cap *= 2; lines = bm_realloc(lines, cap * sizeof *lines); }
            lines[n] = (bm_hdr_line){ s, v, (size_t)(colon - s), (size_t)(ve - v), n };
            n++;
        }
        if (!nl) break;
        s = nl + 1;
    }
    qsort(lines, n, sizeof *lines, bm_hdr_cmp);
    bm_arr out = bm_arr_with_capacity(&bm_type_str, (bm_int)(n * 2));
    bm_str *o = bm_arr_data(out);
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        bool cookie = lines[i].nlen == 10 && strncasecmp(lines[i].name, "set-cookie", 10) == 0;
        if (!cookie)
            while (j < n && lines[j].nlen == lines[i].nlen && strncasecmp(lines[j].name, lines[i].name, lines[i].nlen) == 0) j++;
        char *lower = bm_alloc(lines[i].nlen);
        for (size_t c = 0; c < lines[i].nlen; c++) { char ch = lines[i].name[c]; lower[c] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); }
        o[k++] = bm_str_from(lower, lines[i].nlen);
        bm_free(lower);
        if (j == i + 1) {
            o[k++] = bm_str_from(lines[i].value, lines[i].vlen);
        } else {
            bm_sb v = {0};
            for (size_t x = i; x < j; x++) {
                if (x > i) bm_sb_push(&v, ", ", 2);
                bm_sb_push(&v, lines[x].value, lines[x].vlen);
            }
            o[k++] = bm_str_from_sb(&v);
        }
        i = j;
    }
    out.len = (bm_int)k;
    bm_free(lines);
    return out;
}

/* Each `name` line's value, in order (getSetCookie). */
bm_arr bm_native_headerValues(bm_str block, bm_str name) {
    const char *s = block.p->data, *end = s + block.p->len;
    size_t n = (size_t)name.p->len;
    bm_arr out = BM_EMPTY_ARR;
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *le = nl ? nl : end;
        if ((size_t)(le - s) > n && strncasecmp(s, name.p->data, n) == 0 && s[n] == ':') {
            const char *v = s + n + 1, *ve = le;
            if (ve > v && ve[-1] == '\r') ve--;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            bm_str *slot = bm_arr_reserve_tail(&out, &bm_type_str, 1);
            *slot = bm_str_from(v, (size_t)(ve - v));
            out.len++;
        }
        if (!nl) break;
        s = nl + 1;
    }
    return out;
}

/* TextDecoder's stream mode: how many of the bytes form whole characters, leaving out a character
 * cut off at the end (still valid so far: more bytes may complete it). */
bm_int bm_native_utf8Complete(bm_arr bytes) {
    const uint8_t *p = bm_arr_data(bytes);
    size_t n = (size_t)bytes.len;
    /* look back at most 3 bytes for the start of the last character */
    for (size_t back = 1; back <= 3 && back <= n; back++) {
        uint8_t c = p[n - back];
        if ((c & 0xC0) == 0x80) continue; /* a continuation byte: keep looking */
        size_t need = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 1;
        return (bm_int)(need > back ? n - back : n);
    }
    return (bm_int)n;
}

/* ---- Blob and FormData helpers (bytes kept in strings) */

/* Bytes [start, end) of s, with negative offsets from the end and clamping, as Blob.slice. */
bm_str bm_native_byteSlice(bm_str s, bm_int start, bm_int end) {
    bm_int n = s.p->len;
    if (start < 0) start = start + n < 0 ? 0 : start + n;
    if (end < 0) end = end + n < 0 ? 0 : end + n;
    if (start > n) start = n;
    if (end > n) end = n;
    if (end <= start) return BM_EMPTY_STR;
    return bm_str_from(s.p->data + start, (size_t)(end - start));
}

/* A Blob's type: lower-cased, or "" if it has a byte outside printable ASCII. */
bm_str bm_native_mimeLower(bm_str s) {
    bm_int n = s.p->len;
    for (bm_int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s.p->data[i];
        if (c < 0x20 || c > 0x7e) return BM_EMPTY_STR;
    }
    for (bm_int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s.p->data[i];
        if (c >= 'A' && c <= 'Z') {
            bm_str out = bm_str_from(s.p->data, (size_t)n);
            for (bm_int j = i; j < n; j++) {
                char d = out.p->data[j];
                if (d >= 'A' && d <= 'Z') out.p->data[j] = (char)(d + 32);
            }
            return out;
        }
    }
    bm_str_retain(s);
    return s;
}

/* A multipart/form-data body split into its parts, flattened: [name, filename, content type,
 * "file" or "", value, ...] (a part without a name is skipped), or on a malformed body, one
 * element: what's wrong, as Bun says it. */
bm_arr bm_native_multipartParse(bm_str body, bm_str boundary) {
    bm_arr out = BM_EMPTY_ARR;
    const char *why = NULL;
    size_t bl = (size_t)boundary.p->len + 2;
    char *delim = bm_alloc(bl + 1);
    delim[0] = delim[1] = '-';
    memcpy(delim + 2, boundary.p->data, (size_t)boundary.p->len);
    const char *p = body.p->data, *end = p + body.p->len;
    const char *at = boundary.p->len ? memmem(p, (size_t)(end - p), delim, bl) : NULL;
    for (;;) {
        if (!at) { why = "missing final boundary"; break; }
        const char *part = at + bl;
        if (end - part >= 2 && part[0] == '-' && part[1] == '-') break; /* the closing delimiter */
        if (end - part >= 2 && part[0] == '\r' && part[1] == '\n') part += 2;
        else if (part < end && part[0] == '\n') part += 1;
        const char *next = memmem(part, (size_t)(end - part), delim, bl);
        if (!next) { why = "missing final boundary"; break; }
        const char *stop = next;
        if (stop - part >= 2 && stop[-2] == '\r' && stop[-1] == '\n') stop -= 2;
        else if (stop > part && stop[-1] == '\n') stop -= 1;
        /* headers, a blank line, the value */
        const char *hend = memmem(part, (size_t)(stop - part), "\r\n\r\n", 4);
        size_t skip = 4;
        if (!hend) { hend = memmem(part, (size_t)(stop - part), "\n\n", 2); skip = 2; }
        if (!hend) { why = "is missing header colon separator"; break; }
        const char *name = NULL, *filename = NULL, *type = NULL;
        size_t nl = 0, fl = 0, tl = 0;
        for (const char *l = part; l < hend;) {
            const char *le = memchr(l, '\n', (size_t)(hend - l));
            if (!le) le = hend;
            const char *lend = le > l && le[-1] == '\r' ? le - 1 : le;
            if (lend > l && !memchr(l, ':', (size_t)(lend - l))) { why = "is missing header colon separator"; break; }
            if (lend - l >= 20 && strncasecmp(l, "content-disposition:", 20) == 0) {
                for (const char *x = l + 20; x < lend; x++) {
                    bool isname = lend - x > 6 && strncasecmp(x, "name=\"", 6) == 0 && (x[-1] == ' ' || x[-1] == ';');
                    bool isfile = lend - x > 10 && strncasecmp(x, "filename=\"", 10) == 0;
                    if (!isname && !isfile) continue;
                    const char *v = x + (isfile ? 10 : 6), *ve = memchr(v, '"', (size_t)(lend - v));
                    if (!ve) break;
                    if (isfile) { filename = v; fl = (size_t)(ve - v); } else { name = v; nl = (size_t)(ve - v); }
                    x = ve;
                }
            } else if (lend - l >= 13 && strncasecmp(l, "content-type:", 13) == 0) {
                type = l + 13;
                while (type < lend && *type == ' ') type++;
                tl = (size_t)(lend - type);
            }
            l = le + 1;
        }
        if (why) break;
        if (name) {
            const char *v = hend + skip;
            bm_str fields[5] = {
                bm_str_from(name, nl), filename ? bm_str_from(filename, fl) : BM_EMPTY_STR, type ? bm_str_from(type, tl) : BM_EMPTY_STR,
                filename ? bm_str_from("file", 4) : BM_EMPTY_STR, v < stop ? bm_str_from(v, (size_t)(stop - v)) : BM_EMPTY_STR,
            };
            bm_str *slot = bm_arr_reserve_tail(&out, &bm_type_str, 5);
            memcpy(slot, fields, sizeof fields);
            out.len += 5;
        }
        at = next;
    }
    bm_free(delim);
    if (why) {
        bm_arr_release(out, &bm_type_str);
        out = BM_EMPTY_ARR;
        bm_str *slot = bm_arr_reserve_tail(&out, &bm_type_str, 1);
        *slot = bm_str_from(why, strlen(why));
        out.len = 1;
    }
    return out;
}

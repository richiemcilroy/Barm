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

static _Noreturn void bm_oom(size_t size) {
    bm_out_flush();
    fprintf(stderr, "trap: out of memory (allocating %zu bytes)\n", size);
    fflush(stderr);
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
    fprintf(stderr, "trap: %s\n", msg);
    if (loc && *loc) fprintf(stderr, "  at %s\n", loc);
    fflush(stderr);
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

/* ================================================================== strings */

const bm_strbuf bm_empty_strbuf = {-1, 0, {0}}; /* flexible-array initializer: GNU C (gcc, clang) */

typedef struct { int32_t rc; int32_t len; char data[4]; } bm_small_strbuf;
#define BM_A1(c) {-1, 1, {(char)(c), 0}}
#define BM_A4(c) BM_A1(c), BM_A1((c) + 1), BM_A1((c) + 2), BM_A1((c) + 3)
#define BM_A16(c) BM_A4(c), BM_A4((c) + 4), BM_A4((c) + 8), BM_A4((c) + 12)
/* Immortal one-character ASCII strings: chars()/split("") and friends never allocate for ASCII. */
static const bm_small_strbuf bm_ascii_strs[128] = {
    BM_A16(0), BM_A16(16), BM_A16(32), BM_A16(48), BM_A16(64), BM_A16(80), BM_A16(96), BM_A16(112),
};
#undef BM_A1
#undef BM_A4
#undef BM_A16

BM_STR_LIT(bm_lit_true, "true");
BM_STR_LIT(bm_lit_false, "false");

static inline bm_str bm_ascii_str(unsigned char c) { return (bm_str){(bm_strbuf *)&bm_ascii_strs[c]}; }

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
    if (n == 1 && (unsigned char)bytes[0] < 128) return bm_ascii_str((unsigned char)bytes[0]);
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
        fwrite(bm_out_buf, 1, bm_out_len, stdout);
        bm_out_len = 0;
    }
    fflush(stdout);
}

void bm_out_write(const char *s, size_t n) {
    if (!n) return;
    if (n > bm_out_cap - bm_out_len) {
        bm_out_flush();
        if (!bm_out_big) bm_out_big = bm_alloc(BM_OUT_BIG);
        if (n >= bm_out_cap) {
            fwrite(s, 1, n, stdout);
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
    fwrite(sb->data, 1, sb->len, stderr);
    fflush(stderr);
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
    fwrite(s.p->data, 1, (size_t)s.p->len, stderr);
    fflush(stderr);
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
    bm_timer t = {bm_loop_now() + delay, ++bm_timer_seq, repeat ? delay : 0, ++bm_timer_ids, cb};
    bm_timer_push(t);
    bm_live_timers++;
    return t.id;
}

void bm_clear_timer(bm_int id) {
    for (size_t i = 0; i < bm_ntimers; i++) {
        if (bm_timers[i].id == id && bm_timers[i].cb.fn) {
            bm_env_release(bm_timers[i].cb.env);
            bm_timers[i].cb.fn = NULL;
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
            bm_live_timers--;
            fn(t.cb.env);
            bm_env_release(t.cb.env);
        }
        bm_run_microtasks();
    }
}

static bool bm_http_busy(void);

void bm_async_run(void) {
    for (;;) {
        bm_run_microtasks();
        /* while servers run, their loop runs the timers too */
        if (bm_http_busy()) {
            bm_http_run();
            continue;
        }
        while (bm_ntimers && !bm_timers[0].cb.fn) (void)bm_timer_pop();
        if (!bm_live_timers) return;
        if (bm_timers[0].when > bm_loop_update()) bm_sleep_until(bm_timers[0].when);
        bm_fire_timers();
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
static int bm_http_q = -1;               /* the running loop's kqueue/epoll */
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
#ifdef BM_KQUEUE
    bm_http_q = kqueue();
    struct kevent events[256];
#else
    bm_http_q = epoll_create1(0);
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
    while (bm_http_active > 0 || bm_http_conns) {
        bm_run_microtasks();
        /* connections whose late responses are ready */
        while (bm_http_dirty) {
            bm_http_conn *c = bm_http_dirty;
            bm_http_dirty = c->dirty_next;
            c->dirty = false;
            bm_http_service(c, false, false);
            bm_run_microtasks();
        }
        if (!(bm_http_active > 0 || bm_http_conns)) break;
        if (bm_out_len) bm_out_flush(); /* handler logs reach pipes and files promptly */
        while (bm_ntimers && !bm_timers[0].cb.fn) (void)bm_timer_pop();
        /* wait for I/O, or until the next timer is due */
        bool timed = bm_live_timers > 0;
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
        int n = kevent(bm_http_q, &tch, nch, events, 256, timed && due <= now_ms ? &zero : NULL);
#else
        int timeout = -1;
        if (timed) timeout = due > now_ms ? (int)(due - now_ms) : 0;
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
    }
    close(bm_http_q);
    bm_http_q = -1;
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

static bool bm_http_busy(void) { return bm_http_active > 0 || bm_http_conns; }

void bm_http_run(void) {
    if (!bm_http_busy()) return;
    bm_out_flush();
    bm_int workers = bm_http_want_workers;
    if (workers <= 1) {
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

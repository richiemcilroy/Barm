/* test_runtime.c — standalone tests for the Barm C runtime.
 *
 *   cc -std=c11 -g -fsanitize=address,undefined runtime/test_runtime.c -o /tmp/t -lm && /tmp/t
 *   /tmp/t trap        # exits 101 with "trap: ..." on stderr (checked by runtime/test.sh)
 *
 * Includes barm.c directly (like generated programs do), so internals are reachable.
 * Element ownership is checked with a counting bm_type ("obj") whose retain/release
 * calls are tallied per object: after each group every object must be dead.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1   /* MAP_ANON (glibc) */
#define _DARWIN_C_SOURCE 1  /* MAP_ANON (macOS) */
#include "barm.h"
#include "barm.c"

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/* ================================================================== harness */

static int checks, failures;

#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                         \
    } while (0)

/* Compares and RELEASES s. */
static void check_str_(int line, bm_str s, const char *want, size_t wn) {
    checks++;
    if ((size_t)s.p->len != wn || memcmp(s.p->data, want, wn) != 0 || s.p->data[s.p->len] != 0) {
        failures++;
        fprintf(stderr, "%s:%d: got \"%.*s\", want \"%.*s\"\n", __FILE__, line, (int)s.p->len, s.p->data, (int)wn,
                want);
    }
    bm_str_release(s);
}
#define CHECK_STR(s, want) check_str_(__LINE__, (s), (want), sizeof(want) - 1)
#define CHECK_CSTR(s, want) check_str_(__LINE__, (s), (want), strlen(want))

/* Compares and frees sb. */
static void check_sb_(int line, bm_sb *sb, const char *want) {
    checks++;
    size_t wn = strlen(want);
    if (sb->len != wn || memcmp(sb->data, want, wn) != 0) {
        failures++;
        fprintf(stderr, "%s:%d: got \"%.*s\"\n%*swant \"%s\"\n", __FILE__, line, (int)sb->len, sb->data,
                (int)strlen(__FILE__) + 8, "", want);
    }
    bm_sb_free(sb);
}
#define CHECK_SB(sb, want) check_sb_(__LINE__, &(sb), (want))

/* Compares an array of strings to want[] and RELEASES it. */
static void check_str_arr_(int line, bm_arr a, const char *const *want, int n) {
    checks++;
    bool ok = bm_arr_len(a) == n;
    for (int i = 0; ok && i < n; i++) {
        bm_str s = *(bm_str *)bm_arr_at(a, sizeof(bm_str), i, "test");
        ok = (size_t)s.p->len == strlen(want[i]) && memcmp(s.p->data, want[i], (size_t)s.p->len) == 0;
    }
    if (!ok) {
        failures++;
        bm_sb sb = {0};
        bm_inspect_arr(&sb, a, &bm_type_str, 1);
        fprintf(stderr, "%s:%d: string array mismatch, got %.*s\n", __FILE__, line, (int)sb.len, sb.data);
        bm_sb_free(&sb);
    }
    bm_arr_release(a, &bm_type_str);
}
#define CHECK_STR_ARR(a, ...)                                                      \
    do {                                                                           \
        static const char *const want_[] = {__VA_ARGS__};                          \
        check_str_arr_(__LINE__, (a), want_, (int)(sizeof want_ / sizeof *want_)); \
    } while (0)

/* Strings made by S() are released by drain(). */
static bm_str pool[1 << 16];
static int npool;
static bm_str S(const char *c) {
    bm_str s = bm_str_from(c, strlen(c));
    pool[npool++] = s;
    return s;
}
static void drain(void) {
    while (npool) bm_str_release(pool[--npool]);
}

/* Runs fn with traps redirected here; returns true (and the message) if it trapped. */
static bool traps(void (*fn)(void), char *msg, size_t cap) {
    jmp_buf jb;
    bm_test_jmp = &jb;
    if (setjmp(jb) == 0) {
        fn();
        bm_test_jmp = NULL;
        return false;
    }
    bm_test_jmp = NULL;
    snprintf(msg, cap, "%.*s", (int)bm_test_msg.len, bm_test_msg.data);
    bm_sb_free(&bm_test_msg);
    return true;
}
#define CHECK_TRAPS(fn, want)                                                                                   \
    do {                                                                                                        \
        char m_[256] = "";                                                                                      \
        bool t_ = traps(fn, m_, sizeof m_);                                                                     \
        checks++;                                                                                               \
        if (!t_ || strcmp(m_, want) != 0) {                                                                     \
            failures++;                                                                                         \
            fprintf(stderr, "%s:%d: expected trap \"%s\", got %s \"%s\"\n", __FILE__, __LINE__, want,            \
                    t_ ? "trap" : "no trap", m_);                                                               \
        }                                                                                                       \
    } while (0)

/* Runs fn in a child process; captures stdout/stderr; returns the exit status (or -1). */
static int run_child(void (*fn)(void), char *out, size_t outcap, char *err, size_t errcap) {
    int po[2], pe[2];
    bm_out_flush();
    fflush(stdout);
    fflush(stderr);
    if (pipe(po) || pipe(pe)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(po[1], 1);
        dup2(pe[1], 2);
        close(po[0]), close(po[1]), close(pe[0]), close(pe[1]);
        bm_out_tty = false;
        fn();
        exit(0);
    }
    close(po[1]), close(pe[1]);
    size_t n = 0;
    ssize_t r;
    while ((r = read(po[0], out + n, outcap - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    char sink[4096];
    while (read(po[0], sink, sizeof sink) > 0) {} /* drain overflow */
    n = 0;
    while ((r = read(pe[0], err + n, errcap - 1 - n)) > 0) n += (size_t)r;
    err[n] = 0;
    close(po[0]), close(pe[0]);
    int st;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ================================================================== a counting element type */

typedef int64_t obj;
#define MAXOBJ 8192
static int64_t live[MAXOBJ];
static int64_t n_retain, n_release;
static obj next_obj = 1;

static obj obj_new(void) {
    obj o = next_obj++;
    if (o >= MAXOBJ) abort();
    live[o] = 1;
    return o;
}
static void obj_retain(void *p) { live[*(obj *)p]++, n_retain++; }
static void obj_release(void *p) {
    obj o = *(obj *)p;
    if (live[o] <= 0) {
        failures++;
        fprintf(stderr, "over-release of obj %lld\n", (long long)o);
    }
    live[o]--, n_release++;
}
static bool obj_eq(const void *a, const void *b) { return *(const obj *)a == *(const obj *)b; }
static uint64_t obj_hash(const void *p) { return bm_mix64((uint64_t)*(const obj *)p); }
static void obj_to_str(bm_sb *sb, const void *p) {
    bm_sb_push_char(sb, 'o');
    bm_sb_push_int(sb, *(const obj *)p);
}
static void obj_inspect(bm_sb *sb, const void *p, int d) { (void)d, obj_to_str(sb, p); }
static const bm_type obj_type = {sizeof(obj), obj_retain, obj_release, obj_eq, obj_hash, obj_to_str, obj_inspect};

static void obj_drop(obj o) { obj_release(&o); }
static bool all_dead(void) {
    for (int i = 0; i < MAXOBJ; i++)
        if (live[i]) {
            fprintf(stderr, "obj %d has count %lld\n", i, (long long)live[i]);
            return false;
        }
    return true;
}
static int64_t count_of(obj o) { return live[o]; }

/* ================================================================== strings */

BM_STR_LIT(lit_hello, "hello");

static void test_string_basics(void) {
    /* empty string is immortal and NUL-terminated */
    CHECK(bm_empty_strbuf.rc < 0 && bm_empty_strbuf.len == 0 && bm_empty_strbuf.data[0] == 0);
    bm_str e = bm_str_from("", 0);
    CHECK(e.p == &bm_empty_strbuf);
    bm_str_release(e);
    bm_str_release(e); /* immortal: no-op */

    bm_str a = bm_str_from("héllo", strlen("héllo"));
    CHECK(a.p->rc == 1 && bm_str_byte_len(a) == 6 && a.p->data[6] == 0);
    bm_str_retain(a);
    CHECK(a.p->rc == 2);
    bm_str_release(a);
    CHECK(a.p->rc == 1);

    /* single ASCII characters are interned */
    bm_str x1 = bm_str_from("x", 1), x2 = bm_str_from("x", 1);
    CHECK(x1.p == x2.p && x1.p->rc < 0 && x1.p->data[1] == 0);

    CHECK_STR(bm_str_concat(a, S(" world")), "héllo world");
    bm_str c = bm_str_concat(a, BM_EMPTY_STR);
    CHECK(c.p == a.p && a.p->rc == 2);
    bm_str_release(c);
    c = bm_str_concat(BM_EMPTY_STR, a);
    CHECK(c.p == a.p);
    bm_str_release(c);

    CHECK(bm_str_eq(BM_LIT(lit_hello), S("hello")));
    CHECK(!bm_str_eq(BM_LIT(lit_hello), S("hellO")));
    CHECK(!bm_str_eq(S("a"), S("ab")));
    CHECK(bm_str_cmp(S("a"), S("b")) < 0);
    CHECK(bm_str_cmp(S("b"), S("a")) > 0);
    CHECK(bm_str_cmp(S("abc"), S("abc")) == 0);
    CHECK(bm_str_cmp(S("a"), S("ab")) < 0);
    CHECK(bm_str_cmp(S("ab"), S("a")) > 0);
    CHECK(bm_str_cmp(S("é"), S("z")) > 0);          /* code point order */
    CHECK(bm_str_cmp(S("\xef\xbf\xbf"), S("😀")) < 0); /* U+FFFF < U+1F600 */
    CHECK(bm_str_cmp(BM_EMPTY_STR, S("a")) < 0);

    CHECK(bm_str_hash(BM_LIT(lit_hello)) == bm_str_hash(S("hello")));
    CHECK(bm_str_hash(S("a")) != bm_str_hash(S("b")));
    CHECK(bm_str_hash(BM_EMPTY_STR) != bm_str_hash(bm_str_from("\0", 1)));
    {   /* all lengths 0..40 hash distinctly and consistently */
        char buf[64];
        uint64_t hs[41];
        for (int n = 0; n <= 40; n++) {
            for (int i = 0; i < n; i++) buf[i] = (char)('a' + i % 7);
            buf[n] = 0;
            hs[n] = bm_str_hash(S(buf));
            CHECK(hs[n] == bm_hash_bytes(buf, (size_t)n));
            for (int m = 0; m < n; m++) CHECK(hs[m] != hs[n]);
        }
    }

    CHECK_STR(bm_str_from_int(0), "0");
    CHECK_STR(bm_str_from_int(-42), "-42");
    CHECK_STR(bm_str_from_int(INT64_MAX), "9223372036854775807");
    CHECK_STR(bm_str_from_int(INT64_MIN), "-9223372036854775808");
    CHECK_STR(bm_str_from_bool(true), "true");
    CHECK_STR(bm_str_from_bool(false), "false");

    bm_sb sb = {0};
    bm_sb_push_cstr(&sb, "n=");
    bm_sb_push_int(&sb, -7);
    bm_sb_push_char(&sb, ' ');
    bm_sb_push_f64(&sb, 0.5);
    bm_sb_push_char(&sb, ' ');
    bm_sb_push_str(&sb, BM_LIT(lit_hello));
    bm_sb_push(&sb, "", 0);
    for (int i = 0; i < 1000; i++) bm_sb_push(&sb, "0123456789", 10);
    bm_str big = bm_str_from_sb(&sb);
    CHECK(sb.data == NULL && sb.len == 0 && sb.cap == 0);
    CHECK(big.p->len == 14 + 10000 && memcmp(big.p->data, "n=-7 0.5 hello01", 16) == 0);
    CHECK(big.p->data[big.p->len] == 0 && big.p->rc == 1);
    bm_str_release(big);
    bm_sb e2 = {0};
    CHECK(bm_str_from_sb(&e2).p == &bm_empty_strbuf);
    bm_sb_push_char(&e2, 'q');
    bm_str q = bm_str_from_sb(&e2);
    CHECK(q.p->len == 1 && q.p->data[0] == 'q' && e2.data == NULL);
    bm_sb e3 = {0};
    bm_sb_push_cstr(&e3, "ab");
    CHECK_STR(bm_str_from_sb(&e3), "ab");
    bm_sb e4 = {0};
    bm_sb_push_cstr(&e4, "discard");
    bm_sb_free(&e4);
    bm_sb_free(&e4);

    bm_str_release(a);
    drain();
}

typedef struct { double v; const char *s; } fmt_case;

static void test_number_format(void) {
    /* expected values produced by Node: String(x) */
    static const fmt_case cases[] = {
        {0, "0"}, {1, "1"}, {-1, "-1"}, {42, "42"}, {-0.0, "0"},
        {0.1 + 0.2, "0.30000000000000004"}, {1e21, "1e+21"}, {1e-7, "1e-7"}, {123e-20, "1.23e-18"},
        {5e-324, "5e-324"}, {1.7976931348623157e308, "1.7976931348623157e+308"}, {NAN, "NaN"},
        {INFINITY, "Infinity"}, {-INFINITY, "-Infinity"}, {0.000001, "0.000001"},
        {123456789012345680000.0, "123456789012345680000"}, {0.1, "0.1"}, {1.0 / 3, "0.3333333333333333"},
        {9007199254740992.0, "9007199254740992"}, {9007199254740994.0, "9007199254740994"},
        {-9223372036854775808.0, "-9223372036854776000"}, {9223372036854775808.0, "9223372036854776000"},
        {1e20, "100000000000000000000"}, {1.5e-6, "0.0000015"}, {1234.5678, "1234.5678"}, {100, "100"},
        {1e100, "1e+100"}, {-1e-100, "-1e-100"}, {4.35, "4.35"}, {0.5, "0.5"},
        {2.2250738585072014e-308, "2.2250738585072014e-308"}, {2.225073858507201e-308, "2.225073858507201e-308"},
        {123.456, "123.456"}, {1e16, "10000000000000000"}, {12345678901234567890.0, "12345678901234567000"},
        {-1.5e300, "-1.5e+300"}, {1e-6, "0.000001"}, {1e-5, "0.00001"},
        {999999999999999900000.0, "999999999999999900000"}, {5e-7, "5e-7"}, {1.5, "1.5"}, {-2.5e-8, "-2.5e-8"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        CHECK_CSTR(bm_str_from_f64(cases[i].v), cases[i].s);
        bm_sb sb = {0};
        bm_sb_push_f64(&sb, cases[i].v);
        check_sb_(__LINE__, &sb, cases[i].s);
    }
    /* f32 prints the shortest digits that round-trip as a float */
    float f = 0.1f;
    bm_sb sb = {0};
    bm_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "0.1");
    f = 16777216.0f;
    bm_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "16777216");
    f = 123456789.0f;
    bm_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "123456790");
    f = -0.0f;
    bm_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "0");
    bm_type_f32.inspect(&sb, &f, 1);
    CHECK_SB(sb, "-0");
    f = 1e-10f;
    bm_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "1e-10");

    /* toFixed; expected values from Node x.toFixed(0..5) */
    static const struct { double v; const char *s[6]; } fx[] = {
        {2.5, {"3", "2.5", "2.50", "2.500", "2.5000", "2.50000"}},
        {1.005, {"1", "1.0", "1.00", "1.005", "1.0050", "1.00500"}},
        {0.125, {"0", "0.1", "0.13", "0.125", "0.1250", "0.12500"}},
        {1.45, {"1", "1.4", "1.45", "1.450", "1.4500", "1.45000"}},
        {-1.5, {"-2", "-1.5", "-1.50", "-1.500", "-1.5000", "-1.50000"}},
        {1e21, {"1e+21", "1e+21", "1e+21", "1e+21", "1e+21", "1e+21"}},
        {123.456, {"123", "123.5", "123.46", "123.456", "123.4560", "123.45600"}},
        {-0.0, {"0", "0.0", "0.00", "0.000", "0.0000", "0.00000"}},
        {0.5, {"1", "0.5", "0.50", "0.500", "0.5000", "0.50000"}},
        {-0.5, {"-1", "-0.5", "-0.50", "-0.500", "-0.5000", "-0.50000"}},
        {1.5, {"2", "1.5", "1.50", "1.500", "1.5000", "1.50000"}},
        {-2.5, {"-3", "-2.5", "-2.50", "-2.500", "-2.5000", "-2.50000"}},
        {NAN, {"NaN", "NaN", "NaN", "NaN", "NaN", "NaN"}},
        {INFINITY, {"Infinity", "Infinity", "Infinity", "Infinity", "Infinity", "Infinity"}},
        {-0.0001, {"-0", "-0.0", "-0.00", "-0.000", "-0.0001", "-0.00010"}},
        {1e20,
         {"100000000000000000000", "100000000000000000000.0", "100000000000000000000.00",
          "100000000000000000000.000", "100000000000000000000.0000", "100000000000000000000.00000"}},
        {0.000001, {"0", "0.0", "0.00", "0.000", "0.0000", "0.00000"}},
        {5e-324, {"0", "0.0", "0.00", "0.000", "0.0000", "0.00000"}},
        {9.995, {"10", "10.0", "9.99", "9.995", "9.9950", "9.99500"}},
        {99.5, {"100", "99.5", "99.50", "99.500", "99.5000", "99.50000"}},
    };
    for (size_t i = 0; i < sizeof fx / sizeof *fx; i++)
        for (int d = 0; d <= 5; d++) CHECK_CSTR(bm_f64_to_fixed(fx[i].v, d, "t"), fx[i].s[d]);
    CHECK_STR(bm_f64_to_fixed(1.23, 20, "t"), "1.22999999999999998224");
    CHECK_STR(bm_f64_to_fixed(0.1, 25, "t"), "0.1000000000000000055511151");
    CHECK_STR(bm_f64_to_fixed(123.456, 100, "t"),
              "123.456000000000003069544618483632802963256835937500000000000000000000000000000000000000000000000000000"
              "0");
    CHECK_STR(bm_int_to_fixed(42, 0, "t"), "42");
    CHECK_STR(bm_int_to_fixed(-42, 3, "t"), "-42.000");
    drain();
}

static void trap_to_fixed(void) { bm_str s = bm_f64_to_fixed(1, 101, "f.barm:1:1"); bm_str_release(s); }
static void trap_to_fixed_neg(void) { bm_str s = bm_int_to_fixed(1, -1, "f.barm:1:1"); bm_str_release(s); }

static void test_parse(void) {
    double d = -1;
    CHECK(bm_parse_float(S("3.14"), &d) && d == 3.14);
    CHECK(bm_parse_float(S("  -2.5e3 \n"), &d) && d == -2500);
    CHECK(bm_parse_float(S("\xc2\xa0 1 \xe3\x80\x80\xef\xbb\xbf"), &d) && d == 1);
    CHECK(bm_parse_float(S("Infinity"), &d) && d == INFINITY);
    CHECK(bm_parse_float(S("-Infinity"), &d) && d == -INFINITY);
    CHECK(bm_parse_float(S("+Infinity"), &d) && d == INFINITY);
    CHECK(bm_parse_float(S("+.5"), &d) && d == 0.5);
    CHECK(bm_parse_float(S("5."), &d) && d == 5);
    CHECK(bm_parse_float(S("1E+2"), &d) && d == 100);
    CHECK(bm_parse_float(S("-0"), &d) && d == 0 && signbit(d));
    CHECK(bm_parse_float(S("1e400"), &d) && d == INFINITY);
    CHECK(bm_parse_float(S("0.1"), &d) && d == 0.1);
    CHECK(bm_parse_float(S("123456789012345680000"), &d) && d == 123456789012345680000.0);
    d = 7;
    const char *bad[] = {"", "   ", "1e", "e5", ".", "abc", "1.2.3", "0x10", "inf", "nan", "NaN", "12abc", "1 2",
                         "--1", "+-1", "Infinityx", "infinity", "1e+", "1_000"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!bm_parse_float(S(bad[i]), &d));
    CHECK(d == 7);

    bm_int v = -1;
    CHECK(bm_parse_int(S("42"), 0, &v) && v == 42);
    CHECK(bm_parse_int(S("  -17xyz"), 0, &v) && v == -17);
    CHECK(bm_parse_int(S("0x1F"), 0, &v) && v == 31);
    CHECK(bm_parse_int(S("-0X10"), 0, &v) && v == -16);
    CHECK(bm_parse_int(S("ff"), 16, &v) && v == 255);
    CHECK(bm_parse_int(S("0xff"), 16, &v) && v == 255);
    CHECK(bm_parse_int(S("z"), 36, &v) && v == 35);
    CHECK(bm_parse_int(S("1012"), 2, &v) && v == 5);
    CHECK(bm_parse_int(S("12.5"), 10, &v) && v == 12);
    CHECK(bm_parse_int(S("  +7"), 0, &v) && v == 7);
    CHECK(bm_parse_int(S("0x10"), 10, &v) && v == 0); /* parseInt("0x10", 10) === 0 */
    CHECK(bm_parse_int(S("9223372036854775807"), 10, &v) && v == INT64_MAX);
    CHECK(bm_parse_int(S("-9223372036854775808"), 10, &v) && v == INT64_MIN);
    v = 3;
    CHECK(!bm_parse_int(S("9223372036854775808"), 10, &v));
    CHECK(!bm_parse_int(S("abc"), 0, &v));
    CHECK(!bm_parse_int(S(""), 0, &v));
    CHECK(!bm_parse_int(S("0x"), 0, &v));
    CHECK(!bm_parse_int(S("-"), 0, &v));
    CHECK(!bm_parse_int(S("12"), 1, &v));
    CHECK(!bm_parse_int(S("12"), 37, &v));
    CHECK(!bm_parse_int(S("2"), 2, &v));
    CHECK(v == 3);
    drain();
}

static bm_str g_s;
static void trap_slice(void) { bm_str r = bm_str_slice(g_s, 1, 2, true, "s.barm:4:9"); bm_str_release(r); }
static void trap_slice_start(void) { bm_str r = bm_str_slice(g_s, -1, 0, false, "s.barm:5:1"); bm_str_release(r); }
static void trap_repeat(void) { bm_str r = bm_str_repeat(g_s, -1, "s.barm:6:1"); bm_str_release(r); }
static void trap_repeat_huge(void) { bm_str r = bm_str_repeat(g_s, INT64_MAX, "s.barm:7:1"); bm_str_release(r); }

static void test_string_methods(void) {
    bm_str s = S("héllo😀");
    CHECK(bm_str_byte_len(s) == 10 && bm_str_char_count(s) == 6);
    CHECK(bm_str_char_count(S("")) == 0);
    CHECK(bm_str_char_count(S("aaaaaaaaaaéééééééééé😀😀😀x")) == 24);
    CHECK_STR_ARR(bm_str_chars(s), "h", "é", "l", "l", "o", "😀");
    CHECK(bm_arr_len(bm_str_chars(S(""))) == 0);

    bm_str h = S("hello");
    CHECK_STR(bm_str_slice(h, 1, 3, true, "t"), "el");
    CHECK_STR(bm_str_slice(h, -3, 0, false, "t"), "llo");
    CHECK_STR(bm_str_slice(h, -3, -1, true, "t"), "ll");
    CHECK_STR(bm_str_slice(h, 3, 1, true, "t"), "");
    CHECK_STR(bm_str_slice(h, -100, 2, true, "t"), "he");
    CHECK_STR(bm_str_slice(h, 5, 9, true, "t"), "");
    CHECK_STR(bm_str_slice(h, 0, 100, true, "t"), "hello");
    bm_str whole = bm_str_slice(h, 0, 0, false, "t");
    CHECK(whole.p == h.p && h.p->rc == 2);
    bm_str_release(whole);
    CHECK_STR(bm_str_slice(s, 1, 3, true, "t"), "é");
    CHECK_STR(bm_str_slice(s, -4, 0, false, "t"), "😀");
    g_s = s;
    CHECK_TRAPS(trap_slice, "trap: string index 2 is not on a UTF-8 character boundary");
    CHECK_TRAPS(trap_slice_start, "trap: string index 9 is not on a UTF-8 character boundary");

    CHECK(bm_str_includes(h, S("ell")) && !bm_str_includes(h, S("elo")) && bm_str_includes(h, S("")));
    CHECK(bm_str_starts_with(h, S("he")) && !bm_str_starts_with(h, S("hello!")) && bm_str_starts_with(h, S("")));
    CHECK(bm_str_ends_with(h, S("llo")) && !bm_str_ends_with(h, S("xhello")) && bm_str_ends_with(h, S("")));
    CHECK(bm_str_index_of(h, S("l")) == 2);
    CHECK(bm_str_index_of(h, S("")) == 0);
    CHECK(bm_str_index_of(h, S("xyz")) == -1);
    CHECK(bm_str_index_of(S("aaab"), S("aab")) == 1);
    CHECK(bm_str_index_of(s, S("😀")) == 6);
    CHECK(bm_str_index_of(S("ab"), S("abc")) == -1);

    CHECK_STR_ARR(bm_str_split(S("a,b,,c"), S(",")), "a", "b", "", "c");
    CHECK_STR_ARR(bm_str_split(S("abc"), S("")), "a", "b", "c");
    CHECK_STR_ARR(bm_str_split(S(""), S(",")), "");
    CHECK(bm_arr_len(bm_str_split(S(""), S(""))) == 0);
    CHECK_STR_ARR(bm_str_split(S("a--b--"), S("--")), "a", "b", "");
    CHECK_STR_ARR(bm_str_split(S("--"), S("--")), "", "");
    CHECK_STR_ARR(bm_str_split(S("abc"), S("x")), "abc");
    CHECK_STR_ARR(bm_str_split(S("é😀"), S("")), "é", "😀");
    CHECK_STR_ARR(bm_str_split(S("aaa"), S("aa")), "", "a");

    CHECK_STR(bm_str_trim(S(" \t\n\v\f\r hi there \xc2\xa0\xe3\x80\x80\xef\xbb\xbf\xe2\x80\xa8")), "hi there");
    CHECK_STR(bm_str_trim_start(S("  a b  ")), "a b  ");
    CHECK_STR(bm_str_trim_end(S("  a b  ")), "  a b");
    CHECK_STR(bm_str_trim(S("   ")), "");
    CHECK_STR(bm_str_trim(S("\xe2\x80\x8b x")), "\xe2\x80\x8b x"); /* U+200B is not whitespace */
    CHECK_STR(bm_str_trim(S("é")), "é");
    bm_str same = bm_str_trim(h);
    CHECK(same.p == h.p);
    bm_str_release(same);

    CHECK_STR(bm_str_to_upper(S("abcXYZé1")), "ABCXYZé1");
    CHECK_STR(bm_str_to_lower(S("abcXYZÉ1")), "abcxyzÉ1");
    same = bm_str_to_lower(h);
    CHECK(same.p == h.p);
    bm_str_release(same);

    CHECK_STR(bm_str_replace(S("aXbXc"), S("X"), S("-")), "a-bXc");
    CHECK_STR(bm_str_replace_all(S("aXbXc"), S("X"), S("-")), "a-b-c");
    CHECK_STR(bm_str_replace(S("abc"), S(""), S("-")), "-abc");
    CHECK_STR(bm_str_replace_all(S("abc"), S(""), S("-")), "-a-b-c-");
    CHECK_STR(bm_str_replace_all(S("é😀"), S(""), S("|")), "|é|😀|");
    CHECK_STR(bm_str_replace_all(S(""), S(""), S("x")), "x");
    CHECK_STR(bm_str_replace(S("abc"), S("b"), S("[$&|$$|$`|$'|$1|$]")), "a[b|$|a|c|$1|$]c");
    CHECK_STR(bm_str_replace_all(S("aaa"), S("a"), S("$&$&")), "aaaaaa");
    CHECK_STR(bm_str_replace_all(S("aaaa"), S("aa"), S("b")), "bb");
    CHECK_STR(bm_str_replace_all(S("xyz"), S("xyz"), S("")), "");
    same = bm_str_replace(h, S("q"), S("r"));
    CHECK(same.p == h.p);
    bm_str_release(same);
    same = bm_str_replace_all(h, S("q"), S("r"));
    CHECK(same.p == h.p);
    bm_str_release(same);

    CHECK_STR(bm_str_repeat(S("ab"), 3, "t"), "ababab");
    CHECK_STR(bm_str_repeat(S("abc"), 7, "t"), "abcabcabcabcabcabcabc");
    CHECK_STR(bm_str_repeat(S("x"), 0, "t"), "");
    CHECK_STR(bm_str_repeat(S(""), 5, "t"), "");
    CHECK_STR(bm_str_repeat(h, 1, "t"), "hello");
    g_s = h;
    CHECK_TRAPS(trap_repeat, "trap: Invalid count value: -1");
    CHECK_TRAPS(trap_repeat_huge, "trap: Invalid string length");

    CHECK_STR(bm_str_pad_start(S("5"), 3, S("0")), "005");
    CHECK_STR(bm_str_pad_start(S("abc"), 10, S("123")), "1231231abc");
    CHECK_STR(bm_str_pad_end(S("é"), 4, S("ü")), "éüüü");
    CHECK_STR(bm_str_pad_end(S("a"), 4, S("xy")), "axyx");
    CHECK_STR(bm_str_pad_start(S("😀"), 3, S("é😀")), "é😀😀");
    CHECK_STR(bm_str_pad_start(S("abc"), 2, S("x")), "abc");
    CHECK_STR(bm_str_pad_start(S("abc"), 6, S("")), "abc");
    CHECK_STR(bm_str_pad_end(S("abc"), -5, S("x")), "abc");
    drain();
}

/* ================================================================== arrays */

typedef struct { int64_t key, seq; } rec;
static bool rec_eq(const void *a, const void *b) { return memcmp(a, b, sizeof(rec)) == 0; }
static uint64_t rec_hash(const void *p) { const rec *r = p; return bm_mix64((uint64_t)r->key * 31 + (uint64_t)r->seq); }
static void rec_to_str(bm_sb *sb, const void *p) { bm_sb_push_int(sb, ((const rec *)p)->key); }
static void rec_inspect(bm_sb *sb, const void *p, int d) {
    (void)d;
    bm_sb_push_cstr(sb, "{ key: ");
    bm_sb_push_int(sb, ((const rec *)p)->key);
    bm_sb_push_cstr(sb, ", seq: ");
    bm_sb_push_int(sb, ((const rec *)p)->seq);
    bm_sb_push_cstr(sb, " }");
}
static const bm_type rec_type = {sizeof(rec), NULL, NULL, rec_eq, rec_hash, rec_to_str, rec_inspect};

static int n_cmp_calls;
static double rec_by_key(void *ctx, const void *x, const void *y) {
    (void)ctx;
    n_cmp_calls++;
    return (double)(((const rec *)x)->key - ((const rec *)y)->key);
}
static double int_desc(void *ctx, const void *x, const void *y) {
    *(int *)ctx += 1;
    return (double)(*(const int64_t *)y > *(const int64_t *)x) - (double)(*(const int64_t *)y < *(const int64_t *)x);
}
static double cmp_nan(void *ctx, const void *x, const void *y) { (void)ctx, (void)x, (void)y; return NAN; }

static bm_arr g_arr;
static void trap_at(void) { (void)bm_arr_at(g_arr, sizeof(obj), 3, "a.barm:1:2"); }
static void trap_at_neg(void) { (void)bm_arr_at(g_arr, sizeof(obj), -1, "a.barm:1:3"); }
static void trap_at_mut(void) { (void)bm_arr_at_mut(&g_arr, &obj_type, 10, "a.barm:1:4"); }

static obj oat(bm_arr a, bm_int i) { return *(obj *)bm_arr_at(a, sizeof(obj), i, "t"); }

static void test_array_ownership(void) {
    bm_arr a = BM_EMPTY_ARR;
    CHECK(bm_arr_len(a) == 0 && bm_arr_data(a) == NULL);
    obj first = 0;
    for (int i = 0; i < 10; i++) {
        obj o = obj_new();
        if (!i) first = o;
        bm_arr_push(&a, &obj_type, &o);
        if (i == 0) CHECK(a.p->cap == 4);
        if (i == 4) CHECK(a.p->cap == 8);
    }
    CHECK(bm_arr_len(a) == 10 && a.p->cap == 16 && n_retain == 0);

    /* copy-on-write: b shares a's buffer until b is mutated */
    bm_arr b = a;
    bm_arr_retain(b);
    CHECK(a.p->rc == 2);
    obj extra = obj_new();
    bm_arr_push(&b, &obj_type, &extra);
    CHECK(a.p != b.p && a.p->rc == 1 && b.p->rc == 1);
    CHECK(bm_arr_len(a) == 10 && bm_arr_len(b) == 11);
    CHECK(n_retain == 10 && count_of(first) == 2 && count_of(extra) == 1);
    bm_arr_release(b, &obj_type);
    CHECK(count_of(first) == 1 && count_of(extra) == 0);

    /* mutable access on a shared array clones first */
    b = a;
    bm_arr_retain(b);
    obj *slot = (obj *)bm_arr_at_mut(&b, &obj_type, 0, "t");
    CHECK(b.p != a.p && count_of(first) == 2);
    obj_drop(*slot);
    *slot = obj_new();
    CHECK(oat(a, 0) == first && oat(b, 0) != first && count_of(first) == 1);
    bm_arr_release(b, &obj_type);

    /* pop / shift / unshift, incl. on shared buffers */
    obj out = 0;
    b = a;
    bm_arr_retain(b);
    CHECK(bm_arr_pop(&b, &obj_type, &out) && out == oat(a, 9) && bm_arr_len(b) == 9 && count_of(out) == 2);
    obj_drop(out);
    CHECK(bm_arr_shift(&b, &obj_type, &out) && out == first && oat(b, 0) == oat(a, 1));
    obj_drop(out);
    obj front = obj_new();
    bm_arr_unshift(&b, &obj_type, &front);
    CHECK(oat(b, 0) == front && oat(b, 1) == oat(a, 1) && bm_arr_len(b) == 9);
    CHECK(bm_arr_pop(&b, &obj_type, NULL) && bm_arr_shift(&b, &obj_type, NULL)); /* NULL out releases */
    CHECK(bm_arr_len(b) == 7 && count_of(front) == 0);
    bm_arr_release(b, &obj_type);
    CHECK(bm_arr_len(a) == 10 && oat(a, 0) == first);

    /* bounds traps */
    bm_arr small = bm_arr_slice(a, &obj_type, 0, 3, true, true);
    g_arr = small;
    CHECK_TRAPS(trap_at, "trap: index 3 out of bounds for length 3");
    CHECK_TRAPS(trap_at_neg, "trap: index -1 out of bounds for length 3");
    CHECK_TRAPS(trap_at_mut, "trap: index 10 out of bounds for length 3");
    bm_arr_release(small, &obj_type);

    /* slice: JS semantics incl. negatives */
    bm_arr s1 = bm_arr_slice(a, &obj_type, 1, 3, true, true);
    CHECK(bm_arr_len(s1) == 2 && oat(s1, 0) == oat(a, 1) && count_of(oat(a, 1)) == 2);
    bm_arr s2 = bm_arr_slice(a, &obj_type, -2, 0, true, false);
    CHECK(bm_arr_len(s2) == 2 && oat(s2, 0) == oat(a, 8));
    bm_arr s3 = bm_arr_slice(a, &obj_type, -100, 2, true, true);
    CHECK(bm_arr_len(s3) == 2 && oat(s3, 1) == oat(a, 1));
    bm_arr s4 = bm_arr_slice(a, &obj_type, 3, 1, true, true);
    CHECK(s4.p == NULL);
    bm_arr s5 = bm_arr_slice(a, &obj_type, 0, 0, false, false);
    CHECK(s5.p == a.p && a.p->rc == 2); /* full slice shares the buffer */
    bm_arr s6 = bm_arr_slice(a, &obj_type, 2, -7, true, true);
    CHECK(bm_arr_len(s6) == 1 && oat(s6, 0) == oat(a, 2));
    bm_arr s7 = bm_arr_slice(a, &obj_type, 10, 20, true, true);
    CHECK(bm_arr_len(s7) == 0);
    bm_arr_release(s1, &obj_type);
    bm_arr_release(s2, &obj_type);
    bm_arr_release(s3, &obj_type);
    bm_arr_release(s5, &obj_type);
    bm_arr_release(s6, &obj_type);

    /* concat */
    bm_arr c = bm_arr_concat(a, a, &obj_type);
    CHECK(bm_arr_len(c) == 20 && count_of(first) == 3 && oat(c, 10) == first);
    bm_arr c2 = bm_arr_concat(a, BM_EMPTY_ARR, &obj_type);
    CHECK(c2.p == a.p);
    bm_arr c3 = bm_arr_concat(BM_EMPTY_ARR, a, &obj_type);
    CHECK(c3.p == a.p && a.p->rc == 3);
    bm_arr_release(c, &obj_type);
    bm_arr_release(c2, &obj_type);
    bm_arr_release(c3, &obj_type);

    /* reverse (shared → clone) */
    b = a;
    bm_arr_retain(b);
    bm_arr_reverse(&b, &obj_type);
    CHECK(oat(b, 0) == oat(a, 9) && oat(b, 9) == first && oat(a, 0) == first);
    bm_arr_release(b, &obj_type);

    /* indexOf / lastIndexOf / join / inspect */
    obj o3 = oat(a, 3), missing = MAXOBJ - 1;
    CHECK(bm_arr_index_of(a, &obj_type, &o3) == 3 && bm_arr_last_index_of(a, &obj_type, &o3) == 3);
    CHECK(bm_arr_index_of(a, &obj_type, &missing) == -1 && bm_arr_last_index_of(a, &obj_type, &missing) == -1);
    bm_arr dup = bm_arr_concat(a, a, &obj_type);
    CHECK(bm_arr_index_of(dup, &obj_type, &o3) == 3 && bm_arr_last_index_of(dup, &obj_type, &o3) == 13);
    bm_arr_release(dup, &obj_type);
    bm_arr two = bm_arr_slice(a, &obj_type, 0, 2, true, true);
    bm_str j = bm_arr_join(two, &obj_type, S("-"));
    char want[64];
    snprintf(want, sizeof want, "o%lld-o%lld", (long long)oat(a, 0), (long long)oat(a, 1));
    CHECK_CSTR(j, want);
    bm_arr_release(two, &obj_type);

    /* sort with a shared buffer: the other owner keeps its order */
    b = a;
    bm_arr_retain(b);
    int calls = 0;
    bm_arr_sort(&b, &obj_type, int_desc, &calls); /* obj is an int64 */
    CHECK(calls > 0 && oat(b, 0) == oat(a, 9) && oat(a, 0) == first);
    bm_arr_release(b, &obj_type);

    bm_arr_release(a, &obj_type);
    CHECK(all_dead());
    CHECK(n_retain == n_release - (next_obj - 1)); /* every retain matched; each obj released once more */
    drain();
}

static void test_array_misc(void) {
    /* ints: join / to_str / default sort / comparator sort */
    bm_arr a = BM_EMPTY_ARR;
    int64_t vals[] = {10, 9, 1, 100, 25, -3};
    for (int i = 0; i < 6; i++) bm_arr_push_fast(&a, &bm_type_int, &vals[i]);
    CHECK_STR(bm_arr_join(a, &bm_type_int, S(",")), "10,9,1,100,25,-3");
    CHECK_STR(bm_arr_join(a, &bm_type_int, S("")), "109110025-3");
    bm_sb sb = {0};
    bm_to_str_arr(&sb, a, &bm_type_int);
    CHECK_SB(sb, "10,9,1,100,25,-3");
    bm_arr_sort(&a, &bm_type_int, NULL, NULL); /* JS default: by String(x) */
    CHECK_STR(bm_arr_join(a, &bm_type_int, S(" ")), "-3 1 10 100 25 9");
    int calls = 0;
    bm_arr_sort(&a, &bm_type_int, int_desc, &calls);
    CHECK_STR(bm_arr_join(a, &bm_type_int, S(" ")), "100 25 10 9 1 -3");
    bm_arr_sort(&a, &bm_type_int, cmp_nan, NULL); /* NaN comparator = equal: order kept */
    CHECK_STR(bm_arr_join(a, &bm_type_int, S(" ")), "100 25 10 9 1 -3");
    int64_t nine = 9, seven = 7;
    CHECK(bm_arr_index_of(a, &bm_type_int, &nine) == 3 && bm_arr_index_of(a, &bm_type_int, &seven) == -1);
    bm_arr_reverse(&a, &bm_type_int);
    CHECK_STR(bm_arr_join(a, &bm_type_int, S(" ")), "-3 1 9 10 25 100");
    bm_arr_release(a, &bm_type_int);

    /* stable sort of 16-byte records, large enough to exercise merging */
    bm_arr r = bm_arr_with_capacity(&rec_type, 1000);
    CHECK(r.p->cap == 1000 && bm_arr_len(r) == 0);
    uint64_t x = 12345;
    for (int i = 0; i < 1000; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        rec e = {(int64_t)(x >> 33) % 10, i};
        bm_arr_push_fast(&r, &rec_type, &e);
    }
    CHECK(r.p->cap == 1000);
    bm_arr rs = r;
    bm_arr_retain(rs);
    bm_arr_sort(&rs, &rec_type, rec_by_key, NULL);
    bool stable = true;
    for (int i = 1; i < 1000; i++) {
        const rec *p = bm_arr_at(rs, sizeof(rec), i - 1, "t"), *q = bm_arr_at(rs, sizeof(rec), i, "t");
        if (p->key > q->key || (p->key == q->key && p->seq >= q->seq)) stable = false;
    }
    CHECK(stable);
    CHECK(((const rec *)bm_arr_at(r, sizeof(rec), 0, "t"))->seq == 0); /* original untouched */
    n_cmp_calls = 0;
    bm_arr_sort(&rs, &rec_type, rec_by_key, NULL); /* sorted input: n-1 comparisons */
    CHECK(n_cmp_calls == 999);
    bm_arr_sort(&r, &rec_type, NULL, NULL); /* default: String(key), also stable */
    stable = true;
    for (int i = 1; i < 1000; i++) {
        const rec *p = bm_arr_at(r, sizeof(rec), i - 1, "t"), *q = bm_arr_at(r, sizeof(rec), i, "t");
        if (p->key > q->key || (p->key == q->key && p->seq >= q->seq)) stable = false;
    }
    CHECK(stable);
    bm_arr_release(r, &rec_type);
    bm_arr_release(rs, &rec_type);

    /* strings: default sort, join fast path, release of elements */
    bm_arr ss = BM_EMPTY_ARR;
    const char *words[] = {"pear", "apple", "fig", "Apple", "é", "banana", "apple"};
    for (int i = 0; i < 7; i++) {
        bm_str w = bm_str_from(words[i], strlen(words[i]));
        bm_arr_push(&ss, &bm_type_str, &w);
    }
    bm_arr_sort(&ss, &bm_type_str, NULL, NULL);
    CHECK_STR(bm_arr_join(ss, &bm_type_str, S(",")), "Apple,apple,apple,banana,fig,pear,é");
    bm_str key = S("fig");
    CHECK(bm_arr_index_of(ss, &bm_type_str, &key) == 4);
    bm_arr one = bm_arr_slice(ss, &bm_type_str, 4, 5, true, true);
    bm_str j1 = bm_arr_join(one, &bm_type_str, S(","));
    CHECK(j1.p == ((bm_str *)bm_arr_data(one))->p);
    CHECK_STR(j1, "fig");
    bm_arr_release(one, &bm_type_str);
    bm_arr_release(ss, &bm_type_str);

    /* size-0 elements (arrays of undefined) */
    bm_arr u = BM_EMPTY_ARR;
    char dummy = 0;
    for (int i = 0; i < 5; i++) bm_arr_push(&u, &bm_type_undefined, &dummy);
    CHECK(bm_arr_len(u) == 5);
    CHECK(bm_arr_pop(&u, &bm_type_undefined, &dummy) && bm_arr_shift(&u, &bm_type_undefined, NULL));
    bm_arr_unshift(&u, &bm_type_undefined, &dummy);
    CHECK(bm_arr_len(u) == 4);
    CHECK_STR(bm_arr_join(u, &bm_type_undefined, S(",")), ",,,");
    bm_inspect_arr(&sb, u, &bm_type_undefined, 0);
    CHECK_SB(sb, "[ undefined, undefined, undefined, undefined ]");
    bm_arr_reverse(&u, &bm_type_undefined);
    bm_arr_sort(&u, &bm_type_undefined, NULL, NULL);
    CHECK(bm_arr_index_of(u, &bm_type_undefined, &dummy) == 0);
    bm_arr us = bm_arr_slice(u, &bm_type_undefined, 1, 3, true, true);
    bm_arr uc = bm_arr_concat(u, us, &bm_type_undefined);
    CHECK(bm_arr_len(us) == 2 && bm_arr_len(uc) == 6);
    bm_arr_release(us, &bm_type_undefined);
    bm_arr_release(uc, &bm_type_undefined);
    bm_arr_release(u, &bm_type_undefined);

    /* an immortal (static) array is cloned on mutation and never freed */
    static struct { int64_t rc, cap, pad[2]; int64_t data[3]; } lit = {-1, 3, {0, 0}, {1, 2, 3}};
    bm_arr la = {(bm_arrbuf *)(void *)&lit, 3};
    bm_arr_retain(la);
    bm_arr_release(la, &bm_type_int);
    int64_t four = 4;
    bm_arr_push(&la, &bm_type_int, &four);
    CHECK(la.p != (bm_arrbuf *)(void *)&lit && lit.rc == -1 && lit.data[2] == 3 && bm_arr_len(la) == 4);
    CHECK_STR(bm_arr_join(la, &bm_type_int, S("+")), "1+2+3+4");
    bm_arr_release(la, &bm_type_int);

    /* make_unique / with_capacity edge cases */
    bm_arr e = bm_arr_with_capacity(&bm_type_int, 0);
    CHECK(e.p == NULL);
    bm_arr_make_unique(&e, &bm_type_int, 0);
    CHECK(e.p == NULL);
    bm_arr_make_unique(&e, &bm_type_int, 3);
    CHECK(e.p && e.p->cap == 4 && e.len == 0 && e.p->rc == 1);
    bm_arr e2 = e;
    bm_arr_retain(e2);
    bm_arr_make_unique(&e2, &bm_type_int, 0); /* shared empty buffer → unique empty */
    CHECK(e2.p == NULL && e.p->rc == 1);
    int64_t v = 0;
    CHECK(!bm_arr_pop(&e2, &bm_type_int, &v) && !bm_arr_shift(&e2, &bm_type_int, &v));
    bm_arr_release(e, &bm_type_int);
    bm_arr_release(BM_EMPTY_ARR, &bm_type_int);

    /* arrays of strings share element strings with a clone */
    bm_arr sa = BM_EMPTY_ARR;
    bm_str w = bm_str_from("shared-string", 13);
    bm_arr_push(&sa, &bm_type_str, &w);
    bm_arr sb2 = sa;
    bm_arr_retain(sb2);
    bm_str w2 = bm_str_from("second-string", 13);
    bm_arr_push(&sb2, &bm_type_str, &w2);
    CHECK(w.p->rc == 2);
    bm_arr_release(sa, &bm_type_str);
    CHECK(w.p->rc == 1);
    bm_arr_release(sb2, &bm_type_str);
    drain();
    CHECK(all_dead());
}

/* ================================================================== maps and sets */

static int64_t map_int(bm_map m, int64_t k) {
    int64_t *v = bm_map_get(m, &bm_type_int, &bm_type_int, &k);
    return v ? *v : -999;
}
static void map_set_int(bm_map *m, int64_t k, int64_t v) { bm_map_set(m, &bm_type_int, &bm_type_int, &k, &v); }

static void check_order_(int line, bm_map m, const int64_t *want, int n) {
    bm_arr k = bm_map_keys(m, &bm_type_int, &bm_type_int);
    bool ok = bm_arr_len(k) == n && bm_map_size(m) == n;
    for (int i = 0; ok && i < n; i++) ok = *(int64_t *)bm_arr_at(k, 8, i, "t") == want[i];
    checks++;
    if (!ok) {
        failures++;
        bm_sb sb = {0};
        bm_inspect_arr(&sb, k, &bm_type_int, 1);
        fprintf(stderr, "%s:%d: key order %.*s\n", __FILE__, line, (int)sb.len, sb.data);
        bm_sb_free(&sb);
    }
    bm_arr_release(k, &bm_type_int);
}
#define CHECK_ORDER(m, ...)                                                   \
    do {                                                                      \
        static const int64_t w_[] = {__VA_ARGS__};                            \
        check_order_(__LINE__, (m), w_, (int)(sizeof w_ / sizeof *w_));        \
    } while (0)

static void test_map_basics(void) {
    bm_map m = BM_EMPTY_MAP;
    CHECK(bm_map_size(m) == 0 && map_int(m, 1) == -999);
    int64_t k = 1;
    CHECK(!bm_map_delete(&m, &bm_type_int, &bm_type_int, &k) && m.p == NULL);
    map_set_int(&m, 3, 30);
    map_set_int(&m, 1, 10);
    map_set_int(&m, 2, 20);
    CHECK_ORDER(m, 3, 1, 2);
    map_set_int(&m, 3, 33); /* replace keeps position */
    CHECK_ORDER(m, 3, 1, 2);
    CHECK(map_int(m, 3) == 33 && bm_map_size(m) == 3);
    k = 1;
    CHECK(bm_map_delete(&m, &bm_type_int, &bm_type_int, &k));
    CHECK(!bm_map_delete(&m, &bm_type_int, &bm_type_int, &k));
    CHECK(!bm_map_has(m, &bm_type_int, &bm_type_int, &k));
    map_set_int(&m, 1, 11); /* re-insert goes to the end */
    CHECK_ORDER(m, 3, 2, 1);
    CHECK(map_int(m, 1) == 11);

    /* values and iteration */
    bm_arr vs = bm_map_values(m, &bm_type_int, &bm_type_int);
    CHECK_STR(bm_arr_join(vs, &bm_type_int, S(",")), "33,20,11");
    bm_arr_release(vs, &bm_type_int);
    bm_sb sb = {0};
    void *kp, *vp;
    for (bm_int i = 0; bm_map_next(m, &bm_type_int, &bm_type_int, &i, &kp, &vp);) {
        bm_sb_push_int(&sb, *(int64_t *)kp);
        bm_sb_push_char(&sb, ':');
        bm_sb_push_int(&sb, *(int64_t *)vp);
        bm_sb_push_char(&sb, ' ');
    }
    CHECK_SB(sb, "3:33 2:20 1:11 ");
    bm_int cur = 0;
    CHECK(!bm_map_next(BM_EMPTY_MAP, &bm_type_int, &bm_type_int, &cur, &kp, &vp));

    /* copy-on-write */
    bm_map m2 = m;
    bm_map_retain(m2);
    CHECK(m.p->rc == 2);
    map_set_int(&m2, 4, 40);
    CHECK(m.p != m2.p && m.p->rc == 1 && bm_map_size(m) == 3 && bm_map_size(m2) == 4);
    CHECK_ORDER(m2, 3, 2, 1, 4);
    CHECK(!bm_map_eq(m, m2, &bm_type_int, &bm_type_int));
    k = 4;
    CHECK(bm_map_delete(&m2, &bm_type_int, &bm_type_int, &k));
    CHECK(bm_map_eq(m, m2, &bm_type_int, &bm_type_int));
    map_set_int(&m2, 2, 21);
    CHECK(!bm_map_eq(m, m2, &bm_type_int, &bm_type_int));
    bm_map_release(m2, &bm_type_int, &bm_type_int);

    /* equality ignores insertion order */
    bm_map a = BM_EMPTY_MAP, b = BM_EMPTY_MAP;
    map_set_int(&a, 1, 1), map_set_int(&a, 2, 2);
    map_set_int(&b, 2, 2), map_set_int(&b, 1, 1);
    CHECK(bm_map_eq(a, b, &bm_type_int, &bm_type_int) && bm_map_eq(a, a, &bm_type_int, &bm_type_int));
    CHECK(bm_map_eq(BM_EMPTY_MAP, BM_EMPTY_MAP, &bm_type_int, &bm_type_int));
    CHECK(!bm_map_eq(a, BM_EMPTY_MAP, &bm_type_int, &bm_type_int));
    bm_map_release(a, &bm_type_int, &bm_type_int);
    bm_map_release(b, &bm_type_int, &bm_type_int);

    /* delete on a shared map clones only when the key exists */
    m2 = m;
    bm_map_retain(m2);
    k = 77;
    CHECK(!bm_map_delete(&m2, &bm_type_int, &bm_type_int, &k) && m2.p == m.p);
    k = 3;
    CHECK(bm_map_delete(&m2, &bm_type_int, &bm_type_int, &k) && m2.p != m.p && bm_map_size(m) == 3);
    CHECK_ORDER(m2, 2, 1);
    bm_map_release(m2, &bm_type_int, &bm_type_int);

    /* deleting everything, then reuse */
    for (k = 1; k <= 3; k++) CHECK(bm_map_delete(&m, &bm_type_int, &bm_type_int, &k));
    CHECK(bm_map_size(m) == 0 && m.p->used == 0);
    map_set_int(&m, 9, 90);
    CHECK_ORDER(m, 9);
    bm_map_clear(&m, &bm_type_int, &bm_type_int);
    CHECK(m.p == NULL && bm_map_size(m) == 0);
    bm_map_clear(&m, &bm_type_int, &bm_type_int);
    drain();
}

static void test_map_growth(void) {
    enum { N = 100000 };
    bm_map m = BM_EMPTY_MAP;
    for (int64_t i = 0; i < N; i++) map_set_int(&m, i * 7919, i);
    CHECK(bm_map_size(m) == N && m.p->cap == 131072);
    bool ok = true;
    for (int64_t i = 0; i < N; i++) ok &= map_int(m, i * 7919) == i;
    CHECK(ok);
    CHECK(map_int(m, 1) == -999);
    /* delete 3 of every 4 entries → tombstones */
    for (int64_t i = 0; i < N; i++) {
        int64_t k = i * 7919;
        if (i % 4 != 3) ok &= bm_map_delete(&m, &bm_type_int, &bm_type_int, &k);
    }
    CHECK(ok && bm_map_size(m) == N / 4 && m.p->used == N);
    ok = true;
    for (int64_t i = 0; i < N; i++) ok &= map_int(m, i * 7919) == (i % 4 == 3 ? i : -999);
    CHECK(ok);
    /* fill up: the entry array compacts (same capacity) instead of growing */
    int64_t cap = m.p->cap;
    for (int64_t i = N; m.p->used < cap; i++) map_set_int(&m, i * 7919, i);
    map_set_int(&m, -1, -1);
    CHECK(m.p->cap == cap && m.p->used == m.p->count);
    /* order survives compaction: surviving originals first, then the new keys */
    void *kp, *vp;
    int64_t prev = -1, n = 0;
    ok = true;
    for (bm_int i = 0; bm_map_next(m, &bm_type_int, &bm_type_int, &i, &kp, &vp); n++) {
        int64_t v = *(int64_t *)vp;
        if (n < N / 4) ok &= v % 4 == 3;
        if (n < bm_map_size(m) - 1) ok &= v > prev;
        prev = v;
    }
    CHECK(ok && prev == -1 && n == bm_map_size(m));
    bm_map_release(m, &bm_type_int, &bm_type_int);

    /* small map: compaction when half the entries are deleted */
    bm_map s = BM_EMPTY_MAP;
    for (int64_t k = 0; k < 8; k++) map_set_int(&s, k, k);
    CHECK(s.p->cap == 8);
    for (int64_t k = 0; k < 6; k++) CHECK(bm_map_delete(&s, &bm_type_int, &bm_type_int, &k));
    map_set_int(&s, 100, 100);
    CHECK(s.p->cap == 8 && s.p->used == 3);
    CHECK_ORDER(s, 6, 7, 100);
    map_set_int(&s, 7, 77);
    CHECK_ORDER(s, 6, 7, 100);
    bm_map_release(s, &bm_type_int, &bm_type_int);

    /* randomized differential test against a direct-mapped reference */
    enum { R = 512 };
    static int64_t ref[R];
    bm_map r = BM_EMPTY_MAP;
    for (int k = 0; k < R; k++) ref[k] = -999;
    uint64_t x = 99;
    ok = true;
    for (int step = 0; step < 200000; step++) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        int64_t k = (int64_t)(x % R);
        if ((x >> 20) % 3 == 0) {
            bool had = ref[k] != -999;
            ok &= bm_map_delete(&r, &bm_type_int, &bm_type_int, &k) == had;
            ref[k] = -999;
        } else {
            map_set_int(&r, k, step);
            ref[k] = step;
        }
        if (step % 997 == 0) {
            int64_t n = 0;
            for (int j = 0; j < R; j++) {
                ok &= map_int(r, j) == ref[j];
                n += ref[j] != -999;
            }
            ok &= bm_map_size(r) == n;
        }
    }
    CHECK(ok);
    bm_map_release(r, &bm_type_int, &bm_type_int);
}

static void test_map_ownership(void) {
    /* string keys, counting values */
    bm_map m = BM_EMPTY_MAP;
    char buf[32];
    obj vals[200];
    int64_t retains_before = n_retain;
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof buf, "key-%d", i);
        bm_str k = bm_str_from(buf, strlen(buf));
        vals[i] = obj_new();
        bm_map_set(&m, &bm_type_str, &obj_type, &k, &vals[i]);
    }
    CHECK(bm_map_size(m) == 200 && n_retain == retains_before);
    bm_str probe = S("key-123");
    obj *got = bm_map_get(m, &bm_type_str, &obj_type, &probe);
    CHECK(got && *got == vals[123]);

    /* replacing a value releases the old one and the duplicate key */
    bm_str k2 = bm_str_from("key-5", 5);
    obj nv = obj_new();
    bm_map_set(&m, &bm_type_str, &obj_type, &k2, &nv);
    CHECK(count_of(vals[5]) == 0 && count_of(nv) == 1);

    /* clone retains every key and value; release of the clone balances */
    bm_map c = m;
    bm_map_retain(c);
    bm_str k3 = bm_str_from("new-key", 7);
    obj v3 = obj_new();
    bm_map_set(&c, &bm_type_str, &obj_type, &k3, &v3);
    CHECK(count_of(vals[0]) == 2 && count_of(nv) == 2 && bm_map_size(c) == 201 && bm_map_size(m) == 200);
    bm_arr keys = bm_map_keys(c, &bm_type_str, &obj_type);
    bm_arr values = bm_map_values(c, &bm_type_str, &obj_type);
    CHECK(bm_arr_len(keys) == 201 && count_of(vals[0]) == 3);
    bm_str last = *(bm_str *)bm_arr_at(keys, sizeof(bm_str), 200, "t");
    CHECK(bm_str_eq(last, S("new-key")) && last.p->rc == 2);
    bm_arr_release(keys, &bm_type_str);
    bm_arr_release(values, &obj_type);
    bm_map_release(c, &bm_type_str, &obj_type);
    CHECK(count_of(vals[0]) == 1 && count_of(v3) == 0);

    /* delete releases key and value */
    bm_str dk = S("key-7");
    CHECK(bm_map_delete(&m, &bm_type_str, &obj_type, &dk));
    CHECK(count_of(vals[7]) == 0);
    bm_map_clear(&m, &bm_type_str, &obj_type);
    CHECK(all_dead());

    /* obj keys with a set (size-0 values) */
    bm_map s = BM_EMPTY_MAP;
    obj ks[50];
    for (int i = 0; i < 50; i++) {
        ks[i] = obj_new();
        bm_map_set(&s, &obj_type, &bm_type_undefined, &ks[i], NULL);
    }
    obj dupe = ks[3];
    obj_retain(&dupe);
    bm_map_set(&s, &obj_type, &bm_type_undefined, &dupe, NULL); /* already present: key released */
    CHECK(bm_map_size(s) == 50 && count_of(ks[3]) == 1);
    CHECK(bm_map_has(s, &obj_type, &bm_type_undefined, &ks[10]));
    CHECK(bm_map_get(s, &obj_type, &bm_type_undefined, &ks[10]) != NULL);
    bm_map s2 = s;
    bm_map_retain(s2);
    CHECK(bm_map_delete(&s2, &obj_type, &bm_type_undefined, &ks[10]));
    CHECK(count_of(ks[10]) == 1 && count_of(ks[11]) == 2);
    CHECK(bm_map_has(s, &obj_type, &bm_type_undefined, &ks[10]) &&
          !bm_map_has(s2, &obj_type, &bm_type_undefined, &ks[10]));
    CHECK(!bm_map_eq(s, s2, &obj_type, &bm_type_undefined));
    bm_map_release(s2, &obj_type, &bm_type_undefined);
    bm_map_release(s, &obj_type, &bm_type_undefined);
    CHECK(all_dead());
    drain();
}

static void test_map_keys_special(void) {
    /* f64 keys use SameValueZero: NaN == NaN, 0 == -0 */
    bm_map m = BM_EMPTY_MAP;
    double k = NAN;
    int64_t v = 1;
    bm_map_set(&m, &bm_type_f64, &bm_type_int, &k, &v);
    k = -NAN;
    v = 2;
    bm_map_set(&m, &bm_type_f64, &bm_type_int, &k, &v);
    k = 0.0;
    v = 3;
    bm_map_set(&m, &bm_type_f64, &bm_type_int, &k, &v);
    k = -0.0;
    v = 4;
    bm_map_set(&m, &bm_type_f64, &bm_type_int, &k, &v);
    CHECK(bm_map_size(m) == 2);
    k = NAN;
    CHECK(*(int64_t *)bm_map_get(m, &bm_type_f64, &bm_type_int, &k) == 2);
    k = 0.0;
    CHECK(*(int64_t *)bm_map_get(m, &bm_type_f64, &bm_type_int, &k) == 4);
    bm_sb sb = {0};
    bm_map_inspect(&sb, m, &bm_type_f64, &bm_type_int, 0);
    CHECK_SB(sb, "Map(2) { NaN => 2, 0 => 4 }");
    bm_map_release(m, &bm_type_f64, &bm_type_int);
    /* but === on the descriptor is JS strict equality */
    double n1 = NAN, z1 = 0.0, z2 = -0.0;
    CHECK(!bm_type_f64.eq(&n1, &n1) && bm_type_f64.eq(&z1, &z2));
    CHECK(bm_type_f64.hash(&z1) == bm_type_f64.hash(&z2));
    float fz1 = 0.0f, fz2 = -0.0f;
    CHECK(bm_type_f32.hash(&fz1) == bm_type_f32.hash(&fz2));

    /* small keys (u8) and bool values: padding inside entries */
    bm_map b = BM_EMPTY_MAP;
    for (int i = 0; i < 256; i++) {
        uint8_t key = (uint8_t)i;
        bool val = i % 3 == 0;
        bm_map_set(&b, &bm_type_u8, &bm_type_bool, &key, &val);
    }
    CHECK(bm_map_size(b) == 256);
    uint8_t key = 255;
    CHECK(*(bool *)bm_map_get(b, &bm_type_u8, &bm_type_bool, &key) == true);
    key = 254;
    CHECK(*(bool *)bm_map_get(b, &bm_type_u8, &bm_type_bool, &key) == false);
    bm_map_release(b, &bm_type_u8, &bm_type_bool);

    /* set of strings inspect */
    bm_map s = BM_EMPTY_MAP;
    const char *ws[] = {"b", "a", "b", "it's"};
    for (int i = 0; i < 4; i++) {
        bm_str w = bm_str_from(ws[i], strlen(ws[i]));
        bm_map_set(&s, &bm_type_str, &bm_type_undefined, &w, NULL);
    }
    bm_set_inspect(&sb, s, &bm_type_str, 0);
    CHECK_SB(sb, "Set(3) { 'b', 'a', \"it's\" }");
    bm_arr keys = bm_map_keys(s, &bm_type_str, &bm_type_undefined);
    CHECK_STR_ARR(keys, "b", "a", "it's");
    bm_map_release(s, &bm_type_str, &bm_type_undefined);
}

/* Length-in-value representation: sharing, copy-on-write and exact ownership. */
_Static_assert(sizeof(bm_arrbuf) == 32 && offsetof(bm_arrbuf, data) == 32, "array header is 32 bytes");
_Static_assert(sizeof(bm_arr) == 16, "bm_arr is a pointer + length");

static void trap_reserve_neg(void) { (void)bm_arr_reserve_tail(&g_arr, &bm_type_int, -1); }

static void test_array_len_in_value(void) {
    int64_t r0 = n_retain, x0 = n_release;
    bm_arr a = BM_EMPTY_ARR;
    CHECK(a.p == NULL && a.len == 0);
    obj o[3];
    for (int i = 0; i < 3; i++) {
        o[i] = obj_new();
        bm_arr_push(&a, &obj_type, &o[i]);
    }
    CHECK(a.len == 3 && a.p->cap == 4 && a.p->rc == 1);

    /* sharers of one buffer are exact copies (same len) */
    bm_arr b = a;
    bm_arr_retain(b);
    CHECK(b.p == a.p && b.len == a.len && a.p->rc == 2);
    CHECK(oat(b, 2) == o[2] && n_retain == r0); /* sharing retains the buffer, not the elements */

    /* push on a shared value clones exactly len elements; the other value is unchanged */
    obj extra = obj_new();
    bm_arr_push(&b, &obj_type, &extra);
    CHECK(b.p != a.p && a.p->rc == 1 && b.p->rc == 1 && a.len == 3 && b.len == 4);
    CHECK(n_retain - r0 == 3 && count_of(o[0]) == 2 && count_of(extra) == 1);
    bm_arr_release(b, &obj_type);
    CHECK(count_of(o[0]) == 1 && count_of(extra) == 0 && n_release - x0 == 4);

    /* pop then push on a unique buffer: same buffer, the popped element is no longer owned */
    bm_arrbuf *buf = a.p;
    obj out = 0;
    CHECK(bm_arr_pop(&a, &obj_type, &out) && out == o[2] && a.len == 2 && a.p == buf && a.p->cap == 4);
    obj_drop(out); /* released exactly once: by us, not by the array */
    CHECK(count_of(o[2]) == 0);
    obj o3 = obj_new();
    bm_arr_push(&a, &obj_type, &o3);
    CHECK(a.p == buf && a.len == 3 && oat(a, 2) == o3);

    /* pop on a shared value clones; the original keeps its elements */
    b = a;
    bm_arr_retain(b);
    CHECK(bm_arr_pop(&b, &obj_type, NULL) && b.len == 2 && a.len == 3 && b.p != a.p);
    CHECK(count_of(o3) == 1 && count_of(o[0]) == 2);
    bm_arr_release(b, &obj_type);

    /* a unique buffer with spare capacity after pops: a clone copies only len elements */
    CHECK(bm_arr_pop(&a, &obj_type, NULL) && bm_arr_pop(&a, &obj_type, NULL) && a.len == 1 && a.p->cap == 4);
    CHECK(count_of(o3) == 0 && count_of(o[1]) == 0 && count_of(o[0]) == 1);
    b = a;
    bm_arr_retain(b);
    int64_t r1 = n_retain;
    obj o4 = obj_new();
    bm_arr_push(&b, &obj_type, &o4);
    CHECK(n_retain - r1 == 1 && b.len == 2 && a.len == 1 && oat(b, 0) == o[0] && oat(b, 1) == o4);
    bm_arr_release(b, &obj_type);
    CHECK(count_of(o4) == 0 && count_of(o[0]) == 1);

    /* shift: remaining elements move down; the shifted one moves out */
    obj o5 = obj_new();
    bm_arr_push(&a, &obj_type, &o5);
    CHECK(bm_arr_shift(&a, &obj_type, &out) && out == o[0] && a.len == 1 && oat(a, 0) == o5);
    obj_drop(out);

    /* popping to empty keeps the buffer; releasing it releases nothing more */
    CHECK(bm_arr_pop(&a, &obj_type, NULL) && a.len == 0 && a.p == buf);
    CHECK(!bm_arr_pop(&a, &obj_type, NULL) && !bm_arr_shift(&a, &obj_type, NULL));
    int64_t x1 = n_release;
    bm_arr_release(a, &obj_type);
    CHECK(n_release == x1 && all_dead());

    /* slice / concat return fresh buffers sized to their len, or a copy of the same value */
    a = BM_EMPTY_ARR;
    for (int i = 0; i < 5; i++) {
        obj x = obj_new();
        bm_arr_push(&a, &obj_type, &x);
    }
    bm_arr s = bm_arr_slice(a, &obj_type, 1, -1, true, true);
    CHECK(s.len == 3 && s.p != a.p && s.p->cap == 3 && s.p->rc == 1 && oat(s, 0) == oat(a, 1));
    bm_arr whole = bm_arr_slice(a, &obj_type, 0, 0, true, false);
    CHECK(whole.p == a.p && whole.len == a.len && a.p->rc == 2);
    bm_arr c = bm_arr_concat(s, a, &obj_type);
    CHECK(c.len == 8 && c.p->cap == 8 && oat(c, 3) == oat(a, 0) && count_of(oat(a, 1)) == 4);
    bm_arr c2 = bm_arr_concat(BM_EMPTY_ARR, s, &obj_type);
    CHECK(c2.p == s.p && c2.len == s.len && s.p->rc == 2);
    bm_arr_release(c2, &obj_type);
    bm_arr_release(c, &obj_type);
    bm_arr_release(whole, &obj_type);
    bm_arr_release(s, &obj_type);
    CHECK(count_of(oat(a, 1)) == 1);

    /* reserve_tail: capacity for exactly n more, pointer at element len */
    bm_arr_retain(a); /* shared → reserve_tail clones first */
    b = a;
    obj *tail = (obj *)bm_arr_reserve_tail(&b, &obj_type, 2);
    CHECK(b.p != a.p && b.p->rc == 1 && b.p->cap >= 7 && tail == (obj *)(void *)b.p->data + 5);
    tail[0] = obj_new();
    tail[1] = obj_new();
    b.len += 2;
    CHECK(a.len == 5 && b.len == 7 && count_of(oat(a, 0)) == 2 && count_of(tail[1]) == 1);
    bm_arr_release(b, &obj_type);
    bm_arr_release(a, &obj_type);
    CHECK(all_dead());

    bm_arr r = BM_EMPTY_ARR;
    CHECK(bm_arr_reserve_tail(&r, &bm_type_int, 0) == NULL && r.p == NULL);
    int64_t *w = (int64_t *)bm_arr_reserve_tail(&r, &bm_type_int, 5);
    for (int i = 0; i < 5; i++) w[i] = i * i;
    r.len += 5;
    CHECK(r.p->cap == 5);
    w = (int64_t *)bm_arr_reserve_tail(&r, &bm_type_int, 1); /* unique: grows in place */
    *w = 99;
    r.len++;
    CHECK_STR(bm_arr_join(r, &bm_type_int, S(",")), "0,1,4,9,16,99");
    CHECK(bm_arr_reserve_tail(&r, &bm_type_int, 0) == r.p->data + 6 * sizeof(int64_t));
    g_arr = r;
    CHECK_TRAPS(trap_reserve_neg, "trap: invalid array reserve count");
    bm_arr_release(r, &bm_type_int);

    /* with_capacity + push_fast never reallocates within capacity */
    bm_arr f = bm_arr_with_capacity(&bm_type_int, 3);
    buf = f.p;
    for (int64_t i = 0; i < 3; i++) bm_arr_push_fast(&f, &bm_type_int, &i);
    CHECK(f.p == buf && f.len == 3);
    int64_t three = 3;
    bm_arr_push_fast(&f, &bm_type_int, &three); /* full: falls back to bm_arr_push */
    CHECK(f.len == 4 && f.p->cap == 6);
    bm_arr g = f;
    bm_arr_retain(g);
    bm_arr_push_fast(&g, &bm_type_int, &three); /* shared: clones */
    CHECK(g.p != f.p && g.len == 5 && f.len == 4 && f.p->rc == 1);
    bm_arr_release(g, &bm_type_int);
    bm_arr_release(f, &bm_type_int);

    /* arrays built by the runtime carry their len */
    bm_arr ch = bm_str_chars(S("héllo"));
    CHECK(ch.len == 5 && ch.p->cap == 5);
    bm_arr_release(ch, &bm_type_str);
    bm_map m = BM_EMPTY_MAP;
    for (int64_t i = 0; i < 10; i++) map_set_int(&m, i, i);
    for (int64_t i = 0; i < 10; i += 3) CHECK(bm_map_delete(&m, &bm_type_int, &bm_type_int, &i));
    bm_arr ks = bm_map_keys(m, &bm_type_int, &bm_type_int);
    CHECK(ks.len == 6 && bm_arr_len(ks) == bm_map_size(m));
    CHECK_STR(bm_arr_join(ks, &bm_type_int, S(",")), "1,2,4,5,7,8");
    bm_arr_release(ks, &bm_type_int);
    bm_map_release(m, &bm_type_int, &bm_type_int);

    /* every element retain was matched by a release */
    CHECK(all_dead());
    drain();
}

/* ================================================================== inspect */

static void inspect_arr_int(bm_sb *sb, const void *p, int d) { bm_inspect_arr(sb, *(const bm_arr *)p, &bm_type_int, d); }
static void release_arr_int(void *p) { bm_arr_release(*(bm_arr *)p, &bm_type_int); }
static void retain_arr(void *p) { bm_arr_retain(*(bm_arr *)p); }
static const bm_type arr_int_type = {sizeof(bm_arr), retain_arr, release_arr_int, NULL, NULL, NULL, inspect_arr_int};
static void inspect_arr2(bm_sb *sb, const void *p, int d) { bm_inspect_arr(sb, *(const bm_arr *)p, &arr_int_type, d); }
static void release_arr2(void *p) { bm_arr_release(*(bm_arr *)p, &arr_int_type); }
static const bm_type arr2_type = {sizeof(bm_arr), retain_arr, release_arr2, NULL, NULL, NULL, inspect_arr2};
static void inspect_arr3(bm_sb *sb, const void *p, int d) { bm_inspect_arr(sb, *(const bm_arr *)p, &arr2_type, d); }
static void release_arr3(void *p) { bm_arr_release(*(bm_arr *)p, &arr2_type); }
static const bm_type arr3_type = {sizeof(bm_arr), retain_arr, release_arr3, NULL, NULL, NULL, inspect_arr3};
static void inspect_map_ii(bm_sb *sb, const void *p, int d) {
    bm_map_inspect(sb, *(const bm_map *)p, &bm_type_int, &bm_type_int, d);
}
static void release_map_ii(void *p) { bm_map_release(*(bm_map *)p, &bm_type_int, &bm_type_int); }
static const bm_type map_ii_type = {sizeof(bm_map), NULL, release_map_ii, NULL, NULL, NULL, inspect_map_ii};
static void inspect_set_s(bm_sb *sb, const void *p, int d) { bm_set_inspect(sb, *(const bm_map *)p, &bm_type_str, d); }
static void release_set_s(void *p) { bm_map_release(*(bm_map *)p, &bm_type_str, &bm_type_undefined); }
static const bm_type set_s_type = {sizeof(bm_map), NULL, release_set_s, NULL, NULL, NULL, inspect_set_s};

static bm_arr wrap(const bm_type *t, void *elem) {
    bm_arr a = BM_EMPTY_ARR;
    bm_arr_push(&a, t, elem);
    return a;
}

static void test_inspect(void) {
    bm_sb sb = {0};
    bm_arr a = BM_EMPTY_ARR;
    for (int64_t i = 1; i <= 3; i++) bm_arr_push(&a, &bm_type_int, &i);
    bm_inspect_arr(&sb, a, &bm_type_int, 0);
    CHECK_SB(sb, "[ 1, 2, 3 ]");
    bm_inspect_arr(&sb, BM_EMPTY_ARR, &bm_type_int, 0);
    CHECK_SB(sb, "[]");

    /* strings: unquoted at top level, Node quoting inside containers */
    bm_inspect_str(&sb, S("it's"), 0);
    CHECK_SB(sb, "it's");
    bm_arr ss = BM_EMPTY_ARR;
    const char *words[] = {"a", "it's", "q\"", "both'\"", "all'\"`", "nl\n\ttab\\\x01\x7f\x1b\r\b\f\v", "é", ""};
    for (int i = 0; i < 8; i++) {
        bm_str w = bm_str_from(words[i], strlen(words[i]));
        bm_arr_push(&ss, &bm_type_str, &w);
    }
    bm_inspect_arr(&sb, ss, &bm_type_str, 0);
    CHECK_SB(sb, "[\n  'a',\n  \"it's\",\n  'q\"',\n  `both'\"`,\n  'all\\'\"`',\n"
                 "  'nl\\n\\ttab\\\\\\x01\\x7F\\x1B\\r\\b\\f\\x0B',\n  'é',\n  ''\n]");
    bm_arr_release(ss, &bm_type_str);

    /* numbers, bools, undefined */
    bm_arr f = BM_EMPTY_ARR;
    double ds[] = {-0.0, 0.0, 1.5, NAN, 1e21, -INFINITY};
    for (int i = 0; i < 6; i++) bm_arr_push(&f, &bm_type_f64, &ds[i]);
    bm_inspect_arr(&sb, f, &bm_type_f64, 0);
    CHECK_SB(sb, "[ -0, 0, 1.5, NaN, 1e+21, -Infinity ]");
    bm_to_str_arr(&sb, f, &bm_type_f64);
    CHECK_SB(sb, "0,0,1.5,NaN,1e+21,-Infinity");
    bm_arr_release(f, &bm_type_f64);
    bool t = true;
    bm_type_bool.inspect(&sb, &t, 1);
    bm_type_undefined.inspect(&sb, NULL, 1);
    CHECK_SB(sb, "trueundefined");
    int8_t i8 = -5;
    uint64_t u64 = UINT64_MAX;
    uint32_t u32 = 4000000000u;
    bm_type_i8.inspect(&sb, &i8, 1);
    bm_sb_push_char(&sb, ' ');
    bm_type_u64.to_str(&sb, &u64);
    bm_sb_push_char(&sb, ' ');
    bm_type_u32.to_str(&sb, &u32);
    CHECK_SB(sb, "-5 18446744073709551615 4000000000");
    CHECK(bm_type_int.size == 8 && bm_type_i16.size == 2 && bm_type_u16.size == 2 && bm_type_i32.size == 4 &&
          bm_type_u8.size == 1 && bm_type_bool.size == sizeof(bool) && bm_type_undefined.size == 0 &&
          bm_type_str.retain && bm_type_str.release && !bm_type_int.retain);

    /* nesting: Node shows [Array] below depth 2 */
    bm_arr l1 = a;
    bm_arr_retain(l1);
    bm_arr l2 = wrap(&arr_int_type, &l1);
    bm_arr l3 = wrap(&arr2_type, &l2);
    bm_arr l4 = wrap(&arr3_type, &l3);
    bm_inspect_arr(&sb, l3, &arr2_type, 0);
    CHECK_SB(sb, "[ [ [ 1, 2, 3 ] ] ]");
    bm_inspect_arr(&sb, l4, &arr3_type, 0);
    CHECK_SB(sb, "[ [ [ [Array] ] ] ]");
    bm_arr_release(l4, &arr3_type);

    /* maps and sets */
    bm_map m = BM_EMPTY_MAP;
    bm_str ka = bm_str_from("a", 1), kb = bm_str_from("b", 1);
    int64_t one = 1, two = 2;
    bm_map_set(&m, &bm_type_str, &bm_type_int, &ka, &one);
    bm_map_set(&m, &bm_type_str, &bm_type_int, &kb, &two);
    bm_map_inspect(&sb, m, &bm_type_str, &bm_type_int, 0);
    CHECK_SB(sb, "Map(2) { 'a' => 1, 'b' => 2 }");
    bm_map_release(m, &bm_type_str, &bm_type_int);
    bm_map st = BM_EMPTY_MAP;
    bm_map_set(&st, &bm_type_int, &bm_type_undefined, &one, NULL);
    bm_map_set(&st, &bm_type_int, &bm_type_undefined, &two, NULL);
    bm_map_set(&st, &bm_type_int, &bm_type_undefined, &one, NULL);
    bm_set_inspect(&sb, st, &bm_type_int, 0);
    CHECK_SB(sb, "Set(2) { 1, 2 }");
    bm_map_release(st, &bm_type_int, &bm_type_undefined);
    bm_map_inspect(&sb, BM_EMPTY_MAP, &bm_type_int, &bm_type_int, 0);
    bm_sb_push_char(&sb, ' ');
    bm_set_inspect(&sb, BM_EMPTY_MAP, &bm_type_int, 0);
    CHECK_SB(sb, "Map(0) {} Set(0) {}");

    /* Map nested in arrays: [ [ Map(1) { 1 => 2 } ] ] and [ [ [ [Map] ] ] ] */
    bm_map mi = BM_EMPTY_MAP;
    map_set_int(&mi, 1, 2);
    bm_arr w1 = wrap(&map_ii_type, &mi);
    bm_inspect_arr(&sb, w1, &map_ii_type, 1);
    CHECK_SB(sb, "[ Map(1) { 1 => 2 } ]");
    bm_inspect_arr(&sb, w1, &map_ii_type, 2);
    CHECK_SB(sb, "[ [Map] ]");
    bm_arr_release(w1, &map_ii_type);

    /* Map(1) { 'k' => Set(1) { 'x' } } */
    bm_map inner = BM_EMPTY_MAP;
    bm_str x = bm_str_from("x", 1);
    bm_map_set(&inner, &bm_type_str, &bm_type_undefined, &x, NULL);
    bm_map outer = BM_EMPTY_MAP;
    bm_str kk = bm_str_from("k", 1);
    bm_map_set(&outer, &bm_type_str, &set_s_type, &kk, &inner);
    bm_map_inspect(&sb, outer, &bm_type_str, &set_s_type, 0);
    CHECK_SB(sb, "Map(1) { 'k' => Set(1) { 'x' } }");
    bm_set_inspect(&sb, inner, &bm_type_str, 3);
    CHECK_SB(sb, "[Set]");
    bm_map_release(outer, &bm_type_str, &set_s_type);

    bm_arr_release(a, &bm_type_int);
    drain();
}

/* ================================================================== Node layout (util.inspect) */

/* Expected strings are Node 26's util.inspect output (= console.log), generated from the
 * equivalent JavaScript values. */
static const char want_short_ints[] =
        "[\n"
        "  5,   3,   9, 1,\n"
        "  7, 100, 200\n"
        "]";
static const char want_six_ints[] =
        "[ 1, 2, 3, 4, 5, 6 ]";
static const char want_ints_26[] =
        "[\n"
        "   0,  3,  6,  9, 12, 15, 18, 21,\n"
        "  24, 27, 30, 33, 36, 39, 42, 45,\n"
        "  48, 51, 54, 57, 60, 63, 66, 69,\n"
        "  72, 75\n"
        "]";
static const char want_ints_120[] =
        "[\n"
        "   0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11,\n"
        "  12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,\n"
        "  24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,\n"
        "  36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,\n"
        "  48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,\n"
        "  60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,\n"
        "  72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83,\n"
        "  84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,\n"
        "  96, 97, 98, 99,\n"
        "  ... 20 more items\n"
        "]";
static const char want_f64_101[] =
        "[\n"
        "    0,   1.5,   3,   4.5,   6,   7.5,   9,  10.5,  12,  13.5,\n"
        "   15,  16.5,  18,  19.5,  21,  22.5,  24,  25.5,  27,  28.5,\n"
        "   30,  31.5,  33,  34.5,  36,  37.5,  39,  40.5,  42,  43.5,\n"
        "   45,  46.5,  48,  49.5,  51,  52.5,  54,  55.5,  57,  58.5,\n"
        "   60,  61.5,  63,  64.5,  66,  67.5,  69,  70.5,  72,  73.5,\n"
        "   75,  76.5,  78,  79.5,  81,  82.5,  84,  85.5,  87,  88.5,\n"
        "   90,  91.5,  93,  94.5,  96,  97.5,  99, 100.5, 102, 103.5,\n"
        "  105, 106.5, 108, 109.5, 111, 112.5, 114, 115.5, 117, 118.5,\n"
        "  120, 121.5, 123, 124.5, 126, 127.5, 129, 130.5, 132, 133.5,\n"
        "  135, 136.5, 138, 139.5, 141, 142.5, 144, 145.5, 147, 148.5,\n"
        "  ... 1 more item\n"
        "]";
static const char want_f64_mix[] =
        "[ 0.1, -0, NaN, 1e+21, -Infinity, 123.456, 7, 8 ]";
static const char want_neg_ints_7[] =
        "[\n"
        "    -1, 20, -300,\n"
        "  4000, -5,   60,\n"
        "    -7\n"
        "]";
static const char want_words_7[] =
        "[ 'apple', 'banana', 'cherry', 'date', 'elderberry', 'fig', 'grape' ]";
static const char want_words_30[] =
        "[\n"
        "  'apple',     'banana',     'cherry',\n"
        "  'date',      'elderberry', 'fig',\n"
        "  'grape',     'honeydew',   'kiwi',\n"
        "  'lemon',     'mango',      'nectarine',\n"
        "  'orange',    'papaya',     'quince',\n"
        "  'raspberry', 'strawberry', 'tangerine',\n"
        "  'ugli',      'vanilla',    'watermelon',\n"
        "  'xigua',     'yam',        'zucchini',\n"
        "  'apricot',   'blueberry',  'cantaloupe',\n"
        "  'durian',    'eggplant',   'feijoa'\n"
        "]";
static const char want_long_strs[] =
        "[\n"
        "  'this is a fairly long string number 0',\n"
        "  'this is a fairly long string number 1',\n"
        "  'this is a fairly long string number 2',\n"
        "  'this is a fairly long string number 3',\n"
        "  'this is a fairly long string number 4',\n"
        "  'this is a fairly long string number 5',\n"
        "  'this is a fairly long string number 6'\n"
        "]";
static const char want_multiline_str[] =
        "[ 'first line of a long string\\nsecond line of it\\nthird', 'short' ]";
static const char want_quotes[] =
        "[\n"
        "  \"it's\",\n"
        "  'say \"hi\"',\n"
        "  `it's \"both\"`,\n"
        "  'it\\'s \"both\" `tick`',\n"
        "  'it\\'s \"both\" ${x}',\n"
        "  'back\\\\slash\\ttab',\n"
        "  '\\x85c1'\n"
        "]";
static const char want_wide[] =
        "[\n"
        "  '日本', '中文字',\n"
        "  'a',    'bb',\n"
        "  '😀',   'ccc',\n"
        "  'dddd', 'é'\n"
        "]";
static const char want_bools_8[] =
        "[\n"
        "  true,  false,\n"
        "  true,  true,\n"
        "  false, false,\n"
        "  true,  false\n"
        "]";
static const char want_u8_16[] =
        "[\n"
        "    0,  37, 74, 111, 148, 185,\n"
        "  222,   3, 40,  77, 114, 151,\n"
        "  188, 225,  6,  43\n"
        "]";
static const char want_shapes[] =
        "[\n"
        "  { kind: 'circle', r: 2 },\n"
        "  { kind: 'square', side: 3 },\n"
        "  { kind: 'rect', w: 2, h: 5 }\n"
        "]";
static const char want_one_shape[] =
        "[ { kind: 'circle', r: 2 } ]";
static const char want_shapes_7[] =
        "[\n"
        "  { kind: 'circle', r: 1 },\n"
        "  { kind: 'square', side: 2 },\n"
        "  { kind: 'rect', w: 3, h: 4 },\n"
        "  { kind: 'circle', r: 4 },\n"
        "  { kind: 'square', side: 5 },\n"
        "  { kind: 'rect', w: 6, h: 7 },\n"
        "  { kind: 'circle', r: 7 }\n"
        "]";
static const char want_small_recs_8[] =
        "[\n"
        "  { x: 0 }, { x: 1 },\n"
        "  { x: 2 }, { x: 3 },\n"
        "  { x: 4 }, { x: 5 },\n"
        "  { x: 6 }, { x: 7 }\n"
        "]";
static const char want_nested_4[] =
        "[ [ [ [Array] ], [ [Array] ] ], [ [ [Array] ] ] ]";
static const char want_nested_deep_empty[] =
        "[ [ [ [] ] ] ]";
static const char want_nested_3[] =
        "[ [ [ 1, 2, 3 ] ], [ [ 4 ] ] ]";
static const char want_pairs_10[] =
        "[\n"
        "  [ 0, 0 ],  [ 1, 1 ],\n"
        "  [ 2, 4 ],  [ 3, 9 ],\n"
        "  [ 4, 16 ], [ 5, 25 ],\n"
        "  [ 6, 36 ], [ 7, 49 ],\n"
        "  [ 8, 64 ], [ 9, 81 ]\n"
        "]";
static const char want_map_small[] =
        "Map(2) { 'a' => 1, 'b' => 2 }";
static const char want_map_30[] =
        "Map(30) {\n"
        "  0 => 'apple',\n"
        "  1 => 'banana',\n"
        "  2 => 'cherry',\n"
        "  3 => 'date',\n"
        "  4 => 'elderberry',\n"
        "  5 => 'fig',\n"
        "  6 => 'grape',\n"
        "  7 => 'honeydew',\n"
        "  8 => 'kiwi',\n"
        "  9 => 'lemon',\n"
        "  10 => 'mango',\n"
        "  11 => 'nectarine',\n"
        "  12 => 'orange',\n"
        "  13 => 'papaya',\n"
        "  14 => 'quince',\n"
        "  15 => 'raspberry',\n"
        "  16 => 'strawberry',\n"
        "  17 => 'tangerine',\n"
        "  18 => 'ugli',\n"
        "  19 => 'vanilla',\n"
        "  20 => 'watermelon',\n"
        "  21 => 'xigua',\n"
        "  22 => 'yam',\n"
        "  23 => 'zucchini',\n"
        "  24 => 'apricot',\n"
        "  25 => 'blueberry',\n"
        "  26 => 'cantaloupe',\n"
        "  27 => 'durian',\n"
        "  28 => 'eggplant',\n"
        "  29 => 'feijoa'\n"
        "}";
static const char want_map_arrays[] =
        "Map(2) {\n"
        "  'evens' => [\n"
        "    0,  2,  4,  6,\n"
        "    8, 10, 12, 14\n"
        "  ],\n"
        "  'odds' => [ 1, 3, 5 ]\n"
        "}";
static const char want_map_120[] =
        "Map(120) {\n"
        "  0 => 0,\n"
        "  1 => 2,\n"
        "  2 => 4,\n"
        "  3 => 6,\n"
        "  4 => 8,\n"
        "  5 => 10,\n"
        "  6 => 12,\n"
        "  7 => 14,\n"
        "  8 => 16,\n"
        "  9 => 18,\n"
        "  10 => 20,\n"
        "  11 => 22,\n"
        "  12 => 24,\n"
        "  13 => 26,\n"
        "  14 => 28,\n"
        "  15 => 30,\n"
        "  16 => 32,\n"
        "  17 => 34,\n"
        "  18 => 36,\n"
        "  19 => 38,\n"
        "  20 => 40,\n"
        "  21 => 42,\n"
        "  22 => 44,\n"
        "  23 => 46,\n"
        "  24 => 48,\n"
        "  25 => 50,\n"
        "  26 => 52,\n"
        "  27 => 54,\n"
        "  28 => 56,\n"
        "  29 => 58,\n"
        "  30 => 60,\n"
        "  31 => 62,\n"
        "  32 => 64,\n"
        "  33 => 66,\n"
        "  34 => 68,\n"
        "  35 => 70,\n"
        "  36 => 72,\n"
        "  37 => 74,\n"
        "  38 => 76,\n"
        "  39 => 78,\n"
        "  40 => 80,\n"
        "  41 => 82,\n"
        "  42 => 84,\n"
        "  43 => 86,\n"
        "  44 => 88,\n"
        "  45 => 90,\n"
        "  46 => 92,\n"
        "  47 => 94,\n"
        "  48 => 96,\n"
        "  49 => 98,\n"
        "  50 => 100,\n"
        "  51 => 102,\n"
        "  52 => 104,\n"
        "  53 => 106,\n"
        "  54 => 108,\n"
        "  55 => 110,\n"
        "  56 => 112,\n"
        "  57 => 114,\n"
        "  58 => 116,\n"
        "  59 => 118,\n"
        "  60 => 120,\n"
        "  61 => 122,\n"
        "  62 => 124,\n"
        "  63 => 126,\n"
        "  64 => 128,\n"
        "  65 => 130,\n"
        "  66 => 132,\n"
        "  67 => 134,\n"
        "  68 => 136,\n"
        "  69 => 138,\n"
        "  70 => 140,\n"
        "  71 => 142,\n"
        "  72 => 144,\n"
        "  73 => 146,\n"
        "  74 => 148,\n"
        "  75 => 150,\n"
        "  76 => 152,\n"
        "  77 => 154,\n"
        "  78 => 156,\n"
        "  79 => 158,\n"
        "  80 => 160,\n"
        "  81 => 162,\n"
        "  82 => 164,\n"
        "  83 => 166,\n"
        "  84 => 168,\n"
        "  85 => 170,\n"
        "  86 => 172,\n"
        "  87 => 174,\n"
        "  88 => 176,\n"
        "  89 => 178,\n"
        "  90 => 180,\n"
        "  91 => 182,\n"
        "  92 => 184,\n"
        "  93 => 186,\n"
        "  94 => 188,\n"
        "  95 => 190,\n"
        "  96 => 192,\n"
        "  97 => 194,\n"
        "  98 => 196,\n"
        "  99 => 198,\n"
        "  ... 20 more items\n"
        "}";
static const char want_map_empty[] =
        "Map(0) {}";
static const char want_set_small[] =
        "Set(2) { 1, 2 }";
static const char want_set_words[] =
        "Set(20) {\n"
        "  'apple',\n"
        "  'banana',\n"
        "  'cherry',\n"
        "  'date',\n"
        "  'elderberry',\n"
        "  'fig',\n"
        "  'grape',\n"
        "  'honeydew',\n"
        "  'kiwi',\n"
        "  'lemon',\n"
        "  'mango',\n"
        "  'nectarine',\n"
        "  'orange',\n"
        "  'papaya',\n"
        "  'quince',\n"
        "  'raspberry',\n"
        "  'strawberry',\n"
        "  'tangerine',\n"
        "  'ugli',\n"
        "  'vanilla'\n"
        "}";
static const char want_set_empty[] =
        "Set(0) {}";
static const char want_set_110[] =
        "Set(110) {\n"
        "  0,\n"
        "  1,\n"
        "  2,\n"
        "  3,\n"
        "  4,\n"
        "  5,\n"
        "  6,\n"
        "  7,\n"
        "  8,\n"
        "  9,\n"
        "  10,\n"
        "  11,\n"
        "  12,\n"
        "  13,\n"
        "  14,\n"
        "  15,\n"
        "  16,\n"
        "  17,\n"
        "  18,\n"
        "  19,\n"
        "  20,\n"
        "  21,\n"
        "  22,\n"
        "  23,\n"
        "  24,\n"
        "  25,\n"
        "  26,\n"
        "  27,\n"
        "  28,\n"
        "  29,\n"
        "  30,\n"
        "  31,\n"
        "  32,\n"
        "  33,\n"
        "  34,\n"
        "  35,\n"
        "  36,\n"
        "  37,\n"
        "  38,\n"
        "  39,\n"
        "  40,\n"
        "  41,\n"
        "  42,\n"
        "  43,\n"
        "  44,\n"
        "  45,\n"
        "  46,\n"
        "  47,\n"
        "  48,\n"
        "  49,\n"
        "  50,\n"
        "  51,\n"
        "  52,\n"
        "  53,\n"
        "  54,\n"
        "  55,\n"
        "  56,\n"
        "  57,\n"
        "  58,\n"
        "  59,\n"
        "  60,\n"
        "  61,\n"
        "  62,\n"
        "  63,\n"
        "  64,\n"
        "  65,\n"
        "  66,\n"
        "  67,\n"
        "  68,\n"
        "  69,\n"
        "  70,\n"
        "  71,\n"
        "  72,\n"
        "  73,\n"
        "  74,\n"
        "  75,\n"
        "  76,\n"
        "  77,\n"
        "  78,\n"
        "  79,\n"
        "  80,\n"
        "  81,\n"
        "  82,\n"
        "  83,\n"
        "  84,\n"
        "  85,\n"
        "  86,\n"
        "  87,\n"
        "  88,\n"
        "  89,\n"
        "  90,\n"
        "  91,\n"
        "  92,\n"
        "  93,\n"
        "  94,\n"
        "  95,\n"
        "  96,\n"
        "  97,\n"
        "  98,\n"
        "  99,\n"
        "  ... 10 more items\n"
        "}";
static const char want_person[] =
        "{ name: 'Ada', age: 36, tags: [ 'math', 'engines' ] }";
static const char want_person_long[] =
        "{\n"
        "  name: 'Grace Brewster Murray Hopper',\n"
        "  age: 85,\n"
        "  tags: [\n"
        "    'apple',      'banana',\n"
        "    'cherry',     'date',\n"
        "    'elderberry', 'fig',\n"
        "    'grape',      'honeydew',\n"
        "    'kiwi',       'lemon',\n"
        "    'mango',      'nectarine'\n"
        "  ]\n"
        "}";
static const char want_people[] =
        "[\n"
        "  { name: 'Ada', age: 36, tags: [ 'math' ] },\n"
        "  { name: 'Alan', age: 41, tags: [ 'logic', 'codes' ] },\n"
        "  { name: 'Grace', age: 85, tags: [] }\n"
        "]";
static const char want_nested_rec[] =
        "{ id: 1, pos: { x: 1.5, y: -2 }, items: [ 1, 2, 3 ] }";
static const char want_deep_rec[] =
        "{ a: { b: { c: [Object] } } }";
static const char want_empty_recs[] =
        "[ {}, {} ]";
static const char want_rec_in_arr_deep[] =
        "[ [ [ [Object] ] ] ]";
static const char want_map_in_arr_deep[] =
        "[ [ [ [Map] ] ] ]";
static const char want_set_in_map[] =
        "Map(1) { 'k' => Set(1) { 'x' } }";
static const char want_odd_keys[] =
        "{ '$x': 1, _ok: 2, a1: 3 }";
static const char want_empty_arr[] =
        "[]";
static const char want_rec_with_map[] =
        "{\n"
        "  name: 'inventory',\n"
        "  counts: Map(2) { 'apples' => 3, 'pears' => 12 },\n"
        "  tags: Set(1) { 'fresh' }\n"
        "}";
static const char want_long_ints_row[] =
        "[\n"
        "          1,       10,\n"
        "        100,     1000,\n"
        "      10000,   100000,\n"
        "    1000000, 10000000,\n"
        "  100000000\n"
        "]";
static const char want_arr_of_maps[] =
        "[\n"
        "  Map(1) { 0 => 'apple' },\n"
        "  Map(1) { 1 => 'banana' },\n"
        "  Map(1) { 2 => 'cherry' }\n"
        "]";
#define ARR_TYPE(name, elem)                                                                              \
    static void name##_insp(bm_sb *sb, const void *p, int d) { bm_inspect_arr(sb, *(const bm_arr *)p, elem, d); } \
    static void name##_rel(void *p) { bm_arr_release(*(bm_arr *)p, elem); }                               \
    static const bm_type name = {sizeof(bm_arr), retain_arr, name##_rel, NULL, NULL, NULL, name##_insp};
#define TEST_MAP_TYPE(name, kt, vt)                                                                            \
    static void name##_insp(bm_sb *sb, const void *p, int d) { bm_map_inspect(sb, *(const bm_map *)p, kt, vt, d); } \
    static void name##_rel(void *p) { bm_map_release(*(bm_map *)p, kt, vt); }                             \
    static const bm_type name = {sizeof(bm_map), NULL, name##_rel, NULL, NULL, NULL, name##_insp};

ARR_TYPE(arr_str_type, &bm_type_str)
ARR_TYPE(arr_map_ii_type, &map_ii_type)
ARR_TYPE(arr2_map_ii_type, &arr_map_ii_type)
TEST_MAP_TYPE(map_si_type, &bm_type_str, &bm_type_int)
TEST_MAP_TYPE(map_is_type, &bm_type_int, &bm_type_str)

/* records, as the compiler would generate them */
typedef struct { int64_t kind, a, b; } shape;
BM_STR_LIT(lit_circle, "circle");
BM_STR_LIT(lit_square, "square");
BM_STR_LIT(lit_rect, "rect");
static void shape_insp(bm_sb *sb, const void *p, int d) {
    const shape *s = p;
    static const char *const n0[] = {"kind", "r"}, *const n1[] = {"kind", "side"}, *const n2[] = {"kind", "w", "h"};
    static const bm_type *const ty[] = {&bm_type_str, &bm_type_int, &bm_type_int};
    bm_str kind = s->kind == 0 ? BM_LIT(lit_circle) : s->kind == 1 ? BM_LIT(lit_square) : BM_LIT(lit_rect);
    const void *const f[] = {&kind, &s->a, &s->b};
    bm_inspect_record(sb, d, s->kind == 2 ? 3 : 2, s->kind == 0 ? n0 : s->kind == 1 ? n1 : n2, ty, f);
}
static const bm_type shape_type = {sizeof(shape), NULL, NULL, NULL, NULL, NULL, shape_insp};

typedef struct { int64_t v; } one_int;
#define ONE_INT_TYPE(name, field)                                                   \
    static void name##_insp(bm_sb *sb, const void *p, int d) {                      \
        static const char *const n[] = {field};                                     \
        static const bm_type *const ty[] = {&bm_type_int};                          \
        const void *const f[] = {p};                                                \
        bm_inspect_record(sb, d, 1, n, ty, f);                                       \
    }                                                                               \
    static const bm_type name = {sizeof(one_int), NULL, NULL, NULL, NULL, NULL, name##_insp};
ONE_INT_TYPE(rec_x_type, "x")
ONE_INT_TYPE(rec_a1_type, "a")
ONE_INT_TYPE(rec_d_type, "d")
#define WRAP_TYPE(name, field, inner)                                               \
    static void name##_insp(bm_sb *sb, const void *p, int d) {                      \
        static const char *const n[] = {field};                                     \
        static const bm_type *const ty[] = {inner};                                 \
        const void *const f[] = {p};                                                \
        bm_inspect_record(sb, d, 1, n, ty, f);                                       \
    }                                                                               \
    static const bm_type name = {sizeof(one_int), NULL, NULL, NULL, NULL, NULL, name##_insp};
WRAP_TYPE(rec_c_type, "c", &rec_d_type)
WRAP_TYPE(rec_b_type, "b", &rec_c_type)
WRAP_TYPE(rec_a_type, "a", &rec_b_type)
ARR_TYPE(arr_rec_a1_type, &rec_a1_type)
ARR_TYPE(arr2_rec_a1_type, &arr_rec_a1_type)

static void empty_rec_insp(bm_sb *sb, const void *p, int d) { (void)p, bm_inspect_record(sb, d, 0, NULL, NULL, NULL); }
static const bm_type empty_rec_type = {0, NULL, NULL, NULL, NULL, NULL, empty_rec_insp};

typedef struct { int64_t x, ok, a1; } odd_rec;
static void odd_insp(bm_sb *sb, const void *p, int d) {
    const odd_rec *r = p;
    static const char *const n[] = {"$x", "_ok", "a1"};
    static const bm_type *const ty[] = {&bm_type_int, &bm_type_int, &bm_type_int};
    const void *const f[] = {&r->x, &r->ok, &r->a1};
    bm_inspect_record(sb, d, 3, n, ty, f);
}

typedef struct { bm_str name; int64_t age; bm_arr tags; } person;
static void person_insp(bm_sb *sb, const void *p, int d) {
    const person *r = p;
    static const char *const n[] = {"name", "age", "tags"};
    static const bm_type *const ty[] = {&bm_type_str, &bm_type_int, &arr_str_type};
    const void *const f[] = {&r->name, &r->age, &r->tags};
    bm_inspect_record(sb, d, 3, n, ty, f);
}
static void person_rel(void *p) {
    person *r = p;
    bm_str_release(r->name);
    bm_arr_release(r->tags, &bm_type_str);
}
static const bm_type person_type = {sizeof(person), NULL, person_rel, NULL, NULL, NULL, person_insp};

typedef struct { double x, y; } point;
static void point_insp(bm_sb *sb, const void *p, int d) {
    const point *r = p;
    static const char *const n[] = {"x", "y"};
    static const bm_type *const ty[] = {&bm_type_f64, &bm_type_f64};
    const void *const f[] = {&r->x, &r->y};
    bm_inspect_record(sb, d, 2, n, ty, f);
}
static const bm_type point_type = {sizeof(point), NULL, NULL, NULL, NULL, NULL, point_insp};
typedef struct { int64_t id; point pos; bm_arr items; } item_rec;
static void item_insp(bm_sb *sb, const void *p, int d) {
    const item_rec *r = p;
    static const char *const n[] = {"id", "pos", "items"};
    static const bm_type *const ty[] = {&bm_type_int, &point_type, &arr_int_type};
    const void *const f[] = {&r->id, &r->pos, &r->items};
    bm_inspect_record(sb, d, 3, n, ty, f);
}

typedef struct { bm_str name; bm_map counts, tags; } inventory;
static void inventory_insp(bm_sb *sb, const void *p, int d) {
    const inventory *r = p;
    static const char *const n[] = {"name", "counts", "tags"};
    static const bm_type *const ty[] = {&bm_type_str, &map_si_type, &set_s_type};
    const void *const f[] = {&r->name, &r->counts, &r->tags};
    bm_inspect_record(sb, d, 3, n, ty, f);
}

static const char *const fruit[30] = {
    "apple", "banana", "cherry", "date", "elderberry", "fig", "grape", "honeydew", "kiwi", "lemon",
    "mango", "nectarine", "orange", "papaya", "quince", "raspberry", "strawberry", "tangerine", "ugli", "vanilla",
    "watermelon", "xigua", "yam", "zucchini", "apricot", "blueberry", "cantaloupe", "durian", "eggplant", "feijoa"};

static bm_str mkstr(const char *c) { return bm_str_from(c, strlen(c)); }
static bm_arr strs(const char *const *ws, size_t n) {
    bm_arr a = BM_EMPTY_ARR;
    for (size_t i = 0; i < n; i++) {
        bm_str s = mkstr(ws[i]);
        bm_arr_push(&a, &bm_type_str, &s);
    }
    return a;
}
static bm_arr ints_of(size_t n, const int64_t *v) {
    bm_arr a = BM_EMPTY_ARR;
    for (size_t i = 0; i < n; i++) bm_arr_push(&a, &bm_type_int, (void *)&v[i]);
    return a;
}
static bm_arr one(const bm_type *t, void *elem) { return wrap(t, elem); }

/* Inspects a top-level value (depth 0), compares, releases. */
static void inspect_arr_check(int line, bm_arr a, const bm_type *t, const char *want) {
    bm_sb sb = {0};
    bm_inspect_arr(&sb, a, t, 0);
    check_sb_(line, &sb, want);
    bm_arr_release(a, t);
}
#define INSPECT_ARR(a, t, want) inspect_arr_check(__LINE__, (a), (t), (want))
static void inspect_val_check(int line, const bm_type *t, void *v, const char *want) {
    bm_sb sb = {0};
    t->inspect(&sb, v, 0);
    check_sb_(line, &sb, want);
    if (t->release) t->release(v);
}
#define INSPECT_VAL(t, v, want) inspect_val_check(__LINE__, (t), (v), (want))

static void test_inspect_node_layout(void) {
    bm_arr a;
    int64_t v[128];

    int64_t short_ints[] = {5, 3, 9, 1, 7, 100, 200};
    INSPECT_ARR(ints_of(7, short_ints), &bm_type_int, want_short_ints);
    int64_t six[] = {1, 2, 3, 4, 5, 6};
    INSPECT_ARR(ints_of(6, six), &bm_type_int, want_six_ints);
    for (int i = 0; i < 26; i++) v[i] = i * 3;
    INSPECT_ARR(ints_of(26, v), &bm_type_int, want_ints_26);
    for (int i = 0; i < 120; i++) v[i] = i;
    INSPECT_ARR(ints_of(120, v), &bm_type_int, want_ints_120);
    a = BM_EMPTY_ARR;
    for (int i = 0; i < 101; i++) {
        double x = i * 1.5;
        bm_arr_push(&a, &bm_type_f64, &x);
    }
    INSPECT_ARR(a, &bm_type_f64, want_f64_101);
    double mix[] = {0.1, -0.0, NAN, 1e21, -INFINITY, 123.456, 7, 8};
    a = BM_EMPTY_ARR;
    for (int i = 0; i < 8; i++) bm_arr_push(&a, &bm_type_f64, &mix[i]);
    INSPECT_ARR(a, &bm_type_f64, want_f64_mix);
    int64_t neg[] = {-1, 20, -300, 4000, -5, 60, -7};
    INSPECT_ARR(ints_of(7, neg), &bm_type_int, want_neg_ints_7);
    for (int i = 0; i < 9; i++) v[i] = (int64_t)pow(10, i);
    INSPECT_ARR(ints_of(9, v), &bm_type_int, want_long_ints_row);

    INSPECT_ARR(strs(fruit, 7), &bm_type_str, want_words_7);
    INSPECT_ARR(strs(fruit, 30), &bm_type_str, want_words_30);
    a = BM_EMPTY_ARR;
    for (int i = 0; i < 7; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "this is a fairly long string number %d", i);
        bm_str s = mkstr(buf);
        bm_arr_push(&a, &bm_type_str, &s);
    }
    INSPECT_ARR(a, &bm_type_str, want_long_strs);
    static const char *const ml[] = {"first line of a long string\nsecond line of it\nthird", "short"};
    INSPECT_ARR(strs(ml, 2), &bm_type_str, want_multiline_str);
    static const char *const qs[] = {"it's", "say \"hi\"", "it's \"both\"", "it's \"both\" `tick`",
                                     "it's \"both\" ${x}", "back\\slash\ttab", ("\xc2\x85" "c1")};
    INSPECT_ARR(strs(qs, 7), &bm_type_str, want_quotes);
    static const char *const wide_s[] = {"日本", "中文字", "a", "bb", "😀", "ccc", "dddd", "é"};
    INSPECT_ARR(strs(wide_s, 8), &bm_type_str, want_wide);

    a = BM_EMPTY_ARR;
    bool bs[] = {true, false, true, true, false, false, true, false};
    for (int i = 0; i < 8; i++) bm_arr_push(&a, &bm_type_bool, &bs[i]);
    INSPECT_ARR(a, &bm_type_bool, want_bools_8);
    a = BM_EMPTY_ARR;
    for (int i = 0; i < 16; i++) {
        uint8_t b = (uint8_t)((i * 37) % 256);
        bm_arr_push(&a, &bm_type_u8, &b);
    }
    INSPECT_ARR(a, &bm_type_u8, want_u8_16);

    /* records */
    shape sh[] = {{0, 2, 0}, {1, 3, 0}, {2, 2, 5}};
    a = BM_EMPTY_ARR;
    for (int i = 0; i < 3; i++) bm_arr_push(&a, &shape_type, &sh[i]);
    INSPECT_ARR(a, &shape_type, want_shapes);
    INSPECT_ARR(one(&shape_type, &sh[0]), &shape_type, want_one_shape);
    a = BM_EMPTY_ARR;
    for (int i = 0; i < 7; i++) {
        shape s = {i % 3, i + 1, i + 2};
        bm_arr_push(&a, &shape_type, &s);
    }
    INSPECT_ARR(a, &shape_type, want_shapes_7);
    a = BM_EMPTY_ARR;
    for (int i = 0; i < 8; i++) {
        one_int r = {i};
        bm_arr_push(&a, &rec_x_type, &r);
    }
    INSPECT_ARR(a, &rec_x_type, want_small_recs_8);
    person ada = {mkstr("Ada"), 36, strs((const char *const[]){"math", "engines"}, 2)};
    INSPECT_VAL(&person_type, &ada, want_person);
    person grace = {mkstr("Grace Brewster Murray Hopper"), 85, strs(fruit, 12)};
    INSPECT_VAL(&person_type, &grace, want_person_long);
    a = BM_EMPTY_ARR;
    person ps[] = {{mkstr("Ada"), 36, strs((const char *const[]){"math"}, 1)},
                   {mkstr("Alan"), 41, strs((const char *const[]){"logic", "codes"}, 2)},
                   {mkstr("Grace"), 85, BM_EMPTY_ARR}};
    for (int i = 0; i < 3; i++) bm_arr_push(&a, &person_type, &ps[i]);
    INSPECT_ARR(a, &person_type, want_people);
    int64_t items[] = {1, 2, 3};
    item_rec it = {1, {1.5, -2}, ints_of(3, items)};
    bm_sb sb = {0};
    item_insp(&sb, &it, 0);
    CHECK_SB(sb, want_nested_rec);
    bm_arr_release(it.items, &bm_type_int);
    one_int deep = {1};
    INSPECT_VAL(&rec_a_type, &deep, want_deep_rec);
    char dummy[1] = {0};
    a = BM_EMPTY_ARR;
    bm_arr_push(&a, &empty_rec_type, dummy);
    bm_arr_push(&a, &empty_rec_type, dummy);
    INSPECT_ARR(a, &empty_rec_type, want_empty_recs);
    one_int a1 = {1};
    bm_arr l1 = one(&rec_a1_type, &a1), l2 = one(&arr_rec_a1_type, &l1);
    INSPECT_ARR(one(&arr2_rec_a1_type, &l2), &arr2_rec_a1_type, want_rec_in_arr_deep);
    odd_rec odd = {1, 2, 3};
    odd_insp(&sb, &odd, 0);
    CHECK_SB(sb, want_odd_keys);
    INSPECT_ARR(BM_EMPTY_ARR, &bm_type_int, want_empty_arr);

    /* nested arrays */
    int64_t n12[] = {1, 2}, n3[] = {3}, n456[] = {4, 5, 6};
    bm_arr x12 = ints_of(2, n12), x3 = ints_of(1, n3), x456 = ints_of(3, n456);
    bm_arr y12 = one(&arr_int_type, &x12), y3 = one(&arr_int_type, &x3), y456 = one(&arr_int_type, &x456);
    bm_arr z1 = one(&arr2_type, &y12), z2 = one(&arr2_type, &y456);
    bm_arr_push(&z1, &arr2_type, &y3);
    a = one(&arr3_type, &z1);
    bm_arr_push(&a, &arr3_type, &z2);
    INSPECT_ARR(a, &arr3_type, want_nested_4);
    bm_arr e0 = BM_EMPTY_ARR, e1 = one(&arr_int_type, &e0), e2 = one(&arr2_type, &e1);
    INSPECT_ARR(one(&arr3_type, &e2), &arr3_type, want_nested_deep_empty);
    int64_t n123[] = {1, 2, 3}, n4[] = {4};
    bm_arr p123 = ints_of(3, n123), p4 = ints_of(1, n4);
    bm_arr q123 = one(&arr_int_type, &p123), q4 = one(&arr_int_type, &p4);
    a = one(&arr2_type, &q123);
    bm_arr_push(&a, &arr2_type, &q4);
    INSPECT_ARR(a, &arr2_type, want_nested_3);
    a = BM_EMPTY_ARR;
    for (int i = 0; i < 10; i++) {
        int64_t pr[] = {i, i * i};
        bm_arr p = ints_of(2, pr);
        bm_arr_push(&a, &arr_int_type, &p);
    }
    INSPECT_ARR(a, &arr_int_type, want_pairs_10);

    /* maps and sets */
    bm_map m = BM_EMPTY_MAP;
    for (int i = 0; i < 2; i++) {
        bm_str k = mkstr(i ? "b" : "a");
        int64_t val = i + 1;
        bm_map_set(&m, &bm_type_str, &bm_type_int, &k, &val);
    }
    INSPECT_VAL(&map_si_type, &m, want_map_small);
    m = BM_EMPTY_MAP;
    for (int64_t i = 0; i < 30; i++) {
        bm_str s = mkstr(fruit[i]);
        bm_map_set(&m, &bm_type_int, &bm_type_str, &i, &s);
    }
    INSPECT_VAL(&map_is_type, &m, want_map_30);
    m = BM_EMPTY_MAP;
    int64_t evens[] = {0, 2, 4, 6, 8, 10, 12, 14}, odds[] = {1, 3, 5};
    bm_str ke = mkstr("evens"), ko = mkstr("odds");
    bm_arr ae = ints_of(8, evens), ao = ints_of(3, odds);
    bm_map_set(&m, &bm_type_str, &arr_int_type, &ke, &ae);
    bm_map_set(&m, &bm_type_str, &arr_int_type, &ko, &ao);
    bm_map_inspect(&sb, m, &bm_type_str, &arr_int_type, 0);
    CHECK_SB(sb, want_map_arrays);
    bm_map_release(m, &bm_type_str, &arr_int_type);
    m = BM_EMPTY_MAP;
    for (int64_t i = 0; i < 120; i++) map_set_int(&m, i, i * 2);
    INSPECT_VAL(&map_ii_type, &m, want_map_120);
    m = BM_EMPTY_MAP;
    INSPECT_VAL(&map_ii_type, &m, want_map_empty);
    bm_map s = BM_EMPTY_MAP;
    for (int64_t i = 1; i <= 2; i++) bm_map_set(&s, &bm_type_int, &bm_type_undefined, &i, NULL);
    bm_set_inspect(&sb, s, &bm_type_int, 0);
    CHECK_SB(sb, want_set_small);
    bm_map_release(s, &bm_type_int, &bm_type_undefined);
    s = BM_EMPTY_MAP;
    for (int i = 0; i < 20; i++) {
        bm_str w = mkstr(fruit[i]);
        bm_map_set(&s, &bm_type_str, &bm_type_undefined, &w, NULL);
    }
    INSPECT_VAL(&set_s_type, &s, want_set_words);
    s = BM_EMPTY_MAP;
    INSPECT_VAL(&set_s_type, &s, want_set_empty);
    for (int64_t i = 0; i < 110; i++) bm_map_set(&s, &bm_type_int, &bm_type_undefined, &i, NULL);
    bm_set_inspect(&sb, s, &bm_type_int, 0);
    CHECK_SB(sb, want_set_110);
    bm_map_release(s, &bm_type_int, &bm_type_undefined);
    m = BM_EMPTY_MAP;
    map_set_int(&m, 1, 2);
    bm_arr am = one(&map_ii_type, &m), am2 = one(&arr_map_ii_type, &am);
    INSPECT_ARR(one(&arr2_map_ii_type, &am2), &arr2_map_ii_type, want_map_in_arr_deep);
    bm_map inner = BM_EMPTY_MAP, outer = BM_EMPTY_MAP;
    bm_str xs = mkstr("x"), kk = mkstr("k");
    bm_map_set(&inner, &bm_type_str, &bm_type_undefined, &xs, NULL);
    bm_map_set(&outer, &bm_type_str, &set_s_type, &kk, &inner);
    bm_map_inspect(&sb, outer, &bm_type_str, &set_s_type, 0);
    CHECK_SB(sb, want_set_in_map);
    bm_map_release(outer, &bm_type_str, &set_s_type);
    inventory inv = {mkstr("inventory"), BM_EMPTY_MAP, BM_EMPTY_MAP};
    bm_str ka = mkstr("apples"), kp = mkstr("pears"), kf = mkstr("fresh");
    int64_t three = 3, twelve = 12;
    bm_map_set(&inv.counts, &bm_type_str, &bm_type_int, &ka, &three);
    bm_map_set(&inv.counts, &bm_type_str, &bm_type_int, &kp, &twelve);
    bm_map_set(&inv.tags, &bm_type_str, &bm_type_undefined, &kf, NULL);
    inventory_insp(&sb, &inv, 0);
    CHECK_SB(sb, want_rec_with_map);
    bm_str_release(inv.name);
    bm_map_release(inv.counts, &bm_type_str, &bm_type_int);
    bm_map_release(inv.tags, &bm_type_str, &bm_type_undefined);
    a = BM_EMPTY_ARR;
    for (int64_t i = 0; i < 3; i++) {
        bm_map mi = BM_EMPTY_MAP;
        bm_str w = mkstr(fruit[i]);
        bm_map_set(&mi, &bm_type_int, &bm_type_str, &i, &w);
        bm_arr_push(&a, &map_is_type, &mi);
    }
    INSPECT_ARR(a, &map_is_type, want_arr_of_maps);

    /* the layout state is restored after each value, and reset at depth 0 */
    CHECK(bm_ictx.indent == 0);
    bm_ictx.indent = 40; /* as if an earlier inspect had been interrupted by a trap */
    INSPECT_ARR(ints_of(7, short_ints), &bm_type_int, want_short_ints);
}

/* ================================================================== math, random, closures */

static void trap_nan(void) { (void)bm_f64_to_int(NAN, "Math.floor", "m.barm:1:1"); }
static void trap_big(void) { (void)bm_f64_to_int(1e19, "Math.round", "m.barm:2:1"); }
static void trap_inf(void) { (void)bm_f64_to_int(-INFINITY, "Math.trunc", "m.barm:3:1"); }
static void trap_edge(void) { (void)bm_f64_to_int(9223372036854775808.0, "Math.ceil", "m.barm:4:1"); }

static bool dropped;
static void env_drop(bm_env *e) { (void)e, dropped = true; }

static void test_math_misc(void) {
    CHECK(bm_math_round(2.5) == 3 && bm_math_round(-2.5) == -2 && bm_math_round(1.5) == 2);
    CHECK(bm_math_round(-1.5) == -1 && bm_math_round(0.49999999999999994) == 0);
    CHECK(bm_math_round(-0.4) == 0 && signbit(bm_math_round(-0.4)));
    CHECK(signbit(bm_math_round(-0.0)) && !signbit(bm_math_round(0.4)));
    CHECK(bm_math_round(-0.5) == 0 && signbit(bm_math_round(-0.5)));
    CHECK(bm_math_round(4503599627370497.0) == 4503599627370497.0);
    CHECK(bm_math_round(-4503599627370497.0) == -4503599627370497.0);
    CHECK(isnan(bm_math_round(NAN)) && bm_math_round(INFINITY) == INFINITY);
    CHECK(bm_math_round(2.4999999999999996) == 2 && bm_math_round(-2.5000000000000004) == -3);

    CHECK(bm_f64_to_int(3.0, "Math.floor", "t") == 3 && bm_f64_to_int(-3.7, "x", "t") == -3);
    CHECK(bm_f64_to_int(-9223372036854775808.0, "x", "t") == INT64_MIN);
    CHECK(bm_f64_to_int(9223372036854774784.0, "x", "t") == 9223372036854774784LL);
    CHECK_TRAPS(trap_nan, "trap: Math.floor(NaN) is not an integer");
    CHECK_TRAPS(trap_big, "trap: Math.round(10000000000000000000) is out of the int range");
    CHECK_TRAPS(trap_inf, "trap: Math.trunc(-Infinity) is out of the int range");
    CHECK_TRAPS(trap_edge, "trap: Math.ceil(9223372036854776000) is out of the int range");
    CHECK_TRAPS(trap_to_fixed, "trap: toFixed() digits argument must be between 0 and 100");
    CHECK_TRAPS(trap_to_fixed_neg, "trap: toFixed() digits argument must be between 0 and 100");

    /* random: in [0,1), deterministic for a seed, reasonably spread */
    bm_random_seed(0);
    double first[8];
    for (int i = 0; i < 8; i++) first[i] = bm_random();
    bm_random_seed(0);
    bool same = true;
    for (int i = 0; i < 8; i++) same &= bm_random() == first[i];
    CHECK(same && first[0] != first[1]);
    int buckets[10] = {0};
    bool in_range = true;
    for (int i = 0; i < 100000; i++) {
        double r = bm_random();
        in_range &= r >= 0 && r < 1;
        buckets[(int)(r * 10)]++;
    }
    CHECK(in_range);
    for (int i = 0; i < 10; i++) CHECK(buckets[i] > 9000 && buckets[i] < 11000);

    /* closure environments */
    bm_env *e = bm_alloc(sizeof(bm_env));
    e->rc = 1;
    e->drop = env_drop;
    bm_env_retain(e);
    bm_env_release(e);
    CHECK(!dropped);
    bm_env_release(e);
    CHECK(dropped);
    bm_env_retain(NULL);
    bm_env_release(NULL);
    static bm_env immortal = {-1, env_drop};
    dropped = false;
    bm_env_release(&immortal);
    CHECK(!dropped);
    bm_env *e2 = bm_alloc(sizeof(bm_env));
    e2->rc = 1;
    e2->drop = NULL;
    bm_env_release(e2);

    /* realloc / free wrappers */
    char *p = bm_alloc(0);
    p = bm_realloc(p, 100);
    memset(p, 1, 100);
    bm_free(p);
    CHECK(bm_argc >= 1 && bm_argv != NULL);
}

/* ================================================================== processes: traps, output, runner */

static void child_trap(void) {
    bm_out_write("partial output\n", 15);
    bm_trap("boom", "main.barm:1:2");
}
static void child_trap_math(void) {
    bm_sb sb = {0};
    bm_sb_push_cstr(&sb, "before");
    bm_out_sb_line(&sb);
    (void)bm_f64_to_int(NAN, "Math.floor", "x.barm:2:3");
}
static void child_trap_noloc(void) { bm_trap("no location", NULL); }
static void child_output(void) {
    for (int i = 0; i < 20000; i++) { /* > 64 KiB, crosses buffer flushes */
        bm_sb sb = {0};
        bm_sb_push_cstr(&sb, "line ");
        bm_sb_push_int(&sb, i);
        bm_out_sb_line(&sb);
    }
    char big[70000];
    memset(big, 'z', sizeof big);
    bm_out_write(big, sizeof big); /* larger than the buffer */
    bm_sb sb = {0};
    bm_sb_push_cstr(&sb, "to stderr");
    bm_err_sb_line(&sb);
    bm_out_write("\nend\n", 5);
    exit(0); /* flushed by the atexit handler */
}

static void t_pass(void) {}
static void t_fail_expect(void) {
    bm_sb sb = {0};
    bm_sb_push_cstr(&sb, "expected 3, received 4");
    bm_expect_fail(&sb, "t.barm:3:5");
}
static void t_trap(void) { (void)bm_arr_at(BM_EMPTY_ARR, 8, 0, "t.barm:7:1"); }
static void t_trap_math(void) { (void)bm_f64_to_int(INFINITY, "Math.floor", "t.barm:9:1"); }
static void child_runner(void) {
    bm_test_run("passes", t_pass);
    bm_test_run("fails expect", t_fail_expect);
    bm_test_run("traps", t_trap);
    bm_test_run("traps again", t_trap_math);
    bm_test_run("passes again", t_pass);
    exit(bm_test_summary());
}
static void child_runner_all_pass(void) {
    bm_test_run("only", t_pass);
    exit(bm_test_summary());
}
static void child_expect_outside_test(void) {
    bm_sb sb = {0};
    bm_sb_push_cstr(&sb, "expected 1, received 2");
    bm_expect_fail(&sb, "e.barm:1:1");
}

static char out[1 << 20], err[1 << 16];

static void test_processes(void) {
    int st = run_child(child_trap, out, sizeof out, err, sizeof err);
    CHECK(st == 101);
    CHECK(strcmp(out, "partial output\n") == 0);
    CHECK(strcmp(err, "trap: boom\n  at main.barm:1:2\n") == 0);

    st = run_child(child_trap_math, out, sizeof out, err, sizeof err);
    CHECK(st == 101 && strcmp(out, "before\n") == 0);
    CHECK(strcmp(err, "trap: Math.floor(NaN) is not an integer\n  at x.barm:2:3\n") == 0);

    st = run_child(child_trap_noloc, out, sizeof out, err, sizeof err);
    CHECK(st == 101 && strcmp(err, "trap: no location\n") == 0);

    st = run_child(child_expect_outside_test, out, sizeof out, err, sizeof err);
    CHECK(st == 101 && strcmp(err, "trap: expected 1, received 2\n  at e.barm:1:1\n") == 0);

    st = run_child(child_output, out, sizeof out, err, sizeof err);
    CHECK(st == 0 && strcmp(err, "to stderr\n") == 0);
    CHECK(strncmp(out, "line 0\nline 1\n", 14) == 0 && strstr(out, "line 19999\nzzz") != NULL);
    size_t n = strlen(out);
    size_t want_n = 70000 + 5;
    for (int i = 0; i < 20000; i++) want_n += (size_t)snprintf(NULL, 0, "line %d\n", i);
    CHECK(n > 5 && strcmp(out + n - 5, "\nend\n") == 0 && n == want_n);

    st = run_child(child_runner, out, sizeof out, err, sizeof err);
    CHECK(st == 1);
    const char *want = "ok   passes\n"
                       "FAIL fails expect\n"
                       "  expected 3, received 4\n"
                       "  at t.barm:3:5\n"
                       "FAIL traps\n"
                       "  trap: index 0 out of bounds for length 0\n"
                       "  at t.barm:7:1\n"
                       "FAIL traps again\n"
                       "  trap: Math.floor(Infinity) is out of the int range\n"
                       "  at t.barm:9:1\n"
                       "ok   passes again\n"
                       "2 passed, 3 failed\n";
    checks++;
    if (strcmp(out, want) != 0) {
        failures++;
        fprintf(stderr, "runner output:\n%s---\n", out);
    }
    CHECK(err[0] == 0);

    st = run_child(child_runner_all_pass, out, sizeof out, err, sizeof err);
    CHECK(st == 0 && strcmp(out, "ok   only\n1 passed, 0 failed\n") == 0);
}

/* ================================================================== main */

/* ================================================================== async */

static bm_sb alog;   /* what ran, in order */

static void alog_push(const char *s) {
    if (alog.len) bm_sb_push_char(&alog, ' ');
    bm_sb_push_cstr(&alog, s);
}

static void cb_log(bm_env *env) { alog_push((const char *)((void **)(env + 1))[0]); }

/* A closure logging `word` (an env with the word after the header). */
static bm_fn log_fn(const char *word) {
    struct { bm_env h; const char *w; } *e = bm_alloc(sizeof *e);
    e->h.rc = 1;
    e->h.drop = NULL;
    e->w = word;
    return (bm_fn){(void *)cb_log, &e->h};
}

/* A hand-written async function, as generated code does it: logs `name`, awaits `on` (if any),
 * logs `name` again with a '+' and returns. */
typedef struct {
    void *pc;
    const char *name;
    bm_promise *on;
    char after[32];
} waiter_frame;

static bool waiter_run(bm_task *t) {
    waiter_frame *F = bm_task_frame(t);
    if (F->pc) goto *F->pc;
    alog_push(F->name);
    if (F->on) {
        if (!bm_await_now(F->on)) {
            F->pc = &&resumed;
            bm_await_suspend(F->on);
            return false;
        }
    resumed:;
    }
    snprintf(F->after, sizeof F->after, "%s+", F->name);
    alog_push(F->after);
    bm_promise_resolve_move(t->promise, NULL);
    return true;
}

static bm_task *waiter(const char *name, bm_promise *on) {
    bm_task *t = bm_task_new(sizeof(waiter_frame), waiter_run, &bm_type_undefined);
    waiter_frame *F = bm_task_frame(t);
    F->name = name;
    F->on = on;
    return t;
}

static void test_async(void) {
    /* Microtasks run FIFO, including ones queued while the queue drains. */
    bm_queue_microtask(log_fn("m1"));
    bm_queue_microtask(log_fn("m2"));
    bm_run_microtasks();
    CHECK_SB(alog, "m1 m2");

    /* A spawned task (its caller is still running) yields at an await even when the promise is
     * settled; a task started by the loop continues at once when nothing else is queued. */
    bm_promise *done = bm_promise_new(&bm_type_undefined);
    bm_promise_resolve_move(done, NULL);
    bm_promise *p1 = bm_task_spawn(waiter("a", done));
    alog_push("caller");
    CHECK(p1->state == BM_PENDING);
    bm_run_microtasks();
    CHECK(p1->state == BM_FULFILLED);
    CHECK_SB(alog, "a caller a+");
    bm_task_start(waiter("b", done));
    CHECK_SB(alog, "b b+");
    /* ...but not when a microtask is queued: that one runs first. */
    bm_queue_microtask(log_fn("m"));
    bm_task_start(waiter("c", done));
    bm_run_microtasks();
    CHECK_SB(alog, "c m c+");
    bm_promise_release(p1);

    /* Waiters wake in the order they started waiting, when the promise settles. */
    bm_promise *gate = bm_promise_new(&bm_type_undefined);
    bm_promise *pa = bm_task_spawn(waiter("x", gate));
    bm_promise *pb = bm_task_spawn(waiter("y", gate));
    bm_promise *pc = bm_task_spawn(waiter("z", gate));
    bm_run_microtasks();
    CHECK_SB(alog, "x y z");
    bm_promise_resolve_move(gate, NULL);
    alog_push("resolved");
    bm_run_microtasks();
    CHECK_SB(alog, "resolved x+ y+ z+");
    CHECK(pa->state == BM_FULFILLED && pb->state == BM_FULFILLED && pc->state == BM_FULFILLED);
    bm_promise_release(pa);
    bm_promise_release(pb);
    bm_promise_release(pc);
    bm_promise_release(gate);
    bm_promise_release(done);

    /* Timers fire by due time, then in the order they were set; cleared ones never fire. */
    bm_fn t10 = log_fn("t10"), t1a = log_fn("t1a"), t1b = log_fn("t1b"), tx = log_fn("cleared");
    bm_set_timer(t10, 10, false);
    bm_set_timer(t1a, 1, false);
    bm_set_timer(t1b, 0, false);   /* 0 ms is 1 ms, as in Node */
    bm_clear_timer(bm_set_timer(tx, 2, false));
    bm_env_release(t10.env);
    bm_env_release(t1a.env);
    bm_env_release(t1b.env);
    bm_env_release(tx.env);
    bm_async_run();
    CHECK_SB(alog, "t1a t1b t10");

    /* A promise resolved by a timer wakes its task; values are released once. */
    bm_promise *pv = bm_promise_new(&obj_type);
    obj o = obj_new();
    bm_promise_resolve(pv, &o);
    obj_release(&o);
    CHECK(pv->state == BM_FULFILLED && *(obj *)bm_promise_value(pv) == o);
    bm_promise_release(pv);
    CHECK(all_dead());
}

int main(int argc, char **argv) {
    bm_init(argc, argv);
    if (argc > 1 && strcmp(argv[1], "trap") == 0) { /* used by test.sh to check the exit status */
        bm_out_write("flushed before trap\n", 20);
        (void)bm_arr_at(BM_EMPTY_ARR, 8, 5, "cli.barm:1:1");
    }
    test_string_basics();
    test_number_format();
    test_parse();
    test_string_methods();
    test_array_ownership();
    test_array_misc();
    test_array_len_in_value();
    test_map_basics();
    test_map_growth();
    test_map_ownership();
    test_map_keys_special();
    test_inspect();
    test_inspect_node_layout();
    test_math_misc();
    test_processes();
    test_async();
    bm_sb_free(&alog);
    bm_sb_free(&bm_test_msg);
    CHECK(all_dead());
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

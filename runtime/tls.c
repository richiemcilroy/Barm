/* tls.c — TLS for the fetch client, on the vendored mbedTLS (vendor/mbedtls).
 *
 * The build compiles this file with mbedTLS into an archive that only programs calling fetch()
 * link: their generated code calls bm_tls_install(), which points the runtime's `bm_tls_impl` at
 * the operations below. The runtime never names anything here directly.
 *
 * Certificates are verified against the system's CA bundle (loaded once, on first use):
 * SSL_CERT_FILE, else the first of the usual locations; NODE_EXTRA_CA_CERTS adds more, and a
 * request's own `tls.ca` more still. NODE_TLS_REJECT_UNAUTHORIZED=0 turns verification off, as
 * in Node and Bun. Sessions are remembered per origin, so pooled and later connections resume
 * (TLS 1.2 tickets or IDs, TLS 1.3 tickets) instead of repeating the full handshake. */

#include "barm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

struct bm_tls {
    mbedtls_ssl_context ssl;
    int fd;
    struct bm_tls_conf *conf;
    char key[256];            /* the session cache key (origin + configuration) */
    bool self_signed;         /* the server's certificate signs itself */
    bool chain_self_signed;   /* a certificate above it does (an untrusted root) */
    int depth;                /* certificates the server sent, less one */
    bool verified;
    int last;                 /* the last mbedTLS error */
};

/* A configuration per (verify, extra CA): few in a program. */
typedef struct bm_tls_conf {
    struct bm_tls_conf *next;
    bool verify;
    char *ca;                 /* extra PEM, or NULL */
    size_t ca_len;
    mbedtls_ssl_config cfg;
    mbedtls_x509_crt chain;   /* the roots plus `ca` */
} bm_tls_conf;

static bool bm_tls_ready;
static mbedtls_entropy_context bm_tls_entropy;
static mbedtls_ctr_drbg_context bm_tls_drbg;
static mbedtls_x509_crt bm_tls_roots;
static bm_tls_conf *bm_tls_confs;
static const char *bm_tls_alpn[] = {"http/1.1", NULL};
/* AES-GCM first (the CPU's AES instructions make it ~3x ChaCha20's speed here), as BoringSSL
 * orders them on such hardware; ECDSA before RSA; CBC last, for old servers. */
static const int bm_tls_suites[] = {
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
    MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
    MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,
    0,
};

/* ---- remembered sessions (most recent per key) */

#define BM_TLS_SESSIONS 64
typedef struct { char key[256]; mbedtls_ssl_session s; bool used; unsigned long stamp; } bm_tls_saved;
static bm_tls_saved bm_tls_sessions[BM_TLS_SESSIONS];
static unsigned long bm_tls_clock;

static bm_tls_saved *bm_tls_find(const char *key) {
    for (int i = 0; i < BM_TLS_SESSIONS; i++)
        if (bm_tls_sessions[i].used && strcmp(bm_tls_sessions[i].key, key) == 0) return &bm_tls_sessions[i];
    return NULL;
}

static void bm_tls_save(bm_tls *t) {
    bm_tls_saved *e = bm_tls_find(t->key);
    if (!e) {
        e = &bm_tls_sessions[0];
        for (int i = 0; i < BM_TLS_SESSIONS; i++) {
            if (!bm_tls_sessions[i].used) { e = &bm_tls_sessions[i]; break; }
            if (bm_tls_sessions[i].stamp < e->stamp) e = &bm_tls_sessions[i];
        }
    }
    if (e->used) mbedtls_ssl_session_free(&e->s);
    mbedtls_ssl_session_init(&e->s);
    if (mbedtls_ssl_get_session(&t->ssl, &e->s) != 0) {
        mbedtls_ssl_session_free(&e->s);
        e->used = false;
        return;
    }
    snprintf(e->key, sizeof e->key, "%s", t->key);
    e->used = true;
    e->stamp = ++bm_tls_clock;
}

/* ---- roots */

static char *bm_tls_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (cap - n < 4096) { cap *= 2; buf = realloc(buf, cap); }
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    buf[n] = 0;
    *len = n + 1; /* mbedTLS wants PEM's terminating NUL counted */
    return buf;
}

static void bm_tls_add_pem_file(mbedtls_x509_crt *chain, const char *path) {
    size_t len;
    char *pem = bm_tls_read_file(path, &len);
    if (!pem) return;
    mbedtls_x509_crt_parse(chain, (const unsigned char *)pem, len); /* keeps the certificates it can parse */
    free(pem);
}

static bool bm_tls_init(void) {
    if (bm_tls_ready) return true;
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    mbedtls_entropy_init(&bm_tls_entropy);
    mbedtls_ctr_drbg_init(&bm_tls_drbg);
    if (mbedtls_ctr_drbg_seed(&bm_tls_drbg, mbedtls_entropy_func, &bm_tls_entropy, (const unsigned char *)"barm", 4) != 0) return false;
    mbedtls_x509_crt_init(&bm_tls_roots);
    const char *file = getenv("SSL_CERT_FILE");
    if (file && *file) {
        bm_tls_add_pem_file(&bm_tls_roots, file);
    } else {
        static const char *const bundles[] = {
            "/etc/ssl/cert.pem",                                  /* macOS, Alpine, BSDs */
            "/etc/ssl/certs/ca-certificates.crt",                 /* Debian, Ubuntu, Arch */
            "/etc/pki/tls/certs/ca-bundle.crt",                   /* Fedora, RHEL */
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  /* newer RHEL */
            "/etc/ssl/ca-bundle.pem",                             /* openSUSE */
        };
        for (size_t i = 0; i < sizeof bundles / sizeof *bundles; i++) {
            if (access(bundles[i], R_OK) == 0) { bm_tls_add_pem_file(&bm_tls_roots, bundles[i]); break; }
        }
    }
    const char *extra = getenv("NODE_EXTRA_CA_CERTS");
    if (extra && *extra) bm_tls_add_pem_file(&bm_tls_roots, extra);
    bm_tls_ready = true;
    return true;
}

/* Records whether the server's own certificate is self-signed (for the error message). */
static int bm_tls_verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    bm_tls *t = ctx;
    (void)flags;
    bool self = crt->issuer_raw.len == crt->subject_raw.len && memcmp(crt->issuer_raw.p, crt->subject_raw.p, crt->subject_raw.len) == 0;
    if (depth > t->depth) t->depth = depth;
    if (depth == 0) t->self_signed = self;
    else if (self) t->chain_self_signed = true;
    return 0;
}

static bm_tls_conf *bm_tls_conf_for(bool verify, const char *ca, size_t ca_len) {
    for (bm_tls_conf *c = bm_tls_confs; c; c = c->next)
        if (c->verify == verify && c->ca_len == ca_len && (ca_len == 0 || memcmp(c->ca, ca, ca_len) == 0)) return c;
    bm_tls_conf *c = calloc(1, sizeof *c);
    c->verify = verify;
    mbedtls_ssl_config_init(&c->cfg);
    if (mbedtls_ssl_config_defaults(&c->cfg, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        free(c);
        return NULL;
    }
    mbedtls_ssl_conf_rng(&c->cfg, mbedtls_ctr_drbg_random, &bm_tls_drbg);
    mbedtls_ssl_conf_alpn_protocols(&c->cfg, bm_tls_alpn);
    mbedtls_ssl_conf_ciphersuites(&c->cfg, bm_tls_suites);
    mbedtls_ssl_conf_session_tickets(&c->cfg, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
    mbedtls_ssl_conf_tls13_enable_signal_new_session_tickets(&c->cfg, MBEDTLS_SSL_TLS1_3_SIGNAL_NEW_SESSION_TICKETS_ENABLED);
    mbedtls_ssl_conf_authmode(&c->cfg, verify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_x509_crt_init(&c->chain);
    if (ca_len) {
        c->ca = malloc(ca_len + 1);
        memcpy(c->ca, ca, ca_len);
        c->ca[ca_len] = 0;
        c->ca_len = ca_len;
        mbedtls_x509_crt_parse(&c->chain, (const unsigned char *)c->ca, ca_len + 1);
        /* the system roots too: a request's `ca` adds to them */
        for (mbedtls_x509_crt *r = &bm_tls_roots; r && r->raw.len; r = r->next) mbedtls_x509_crt_parse_der(&c->chain, r->raw.p, r->raw.len);
        mbedtls_ssl_conf_ca_chain(&c->cfg, &c->chain, NULL);
    } else {
        mbedtls_ssl_conf_ca_chain(&c->cfg, &bm_tls_roots, NULL);
    }
    c->next = bm_tls_confs;
    bm_tls_confs = c;
    return c;
}

/* ---- I/O on the socket (non-blocking) */

static int bm_tls_send(void *ctx, const unsigned char *buf, size_t len) {
    bm_tls *t = ctx;
#ifdef MSG_NOSIGNAL
    ssize_t n = send(t->fd, buf, len, MSG_NOSIGNAL);
#else
    ssize_t n = send(t->fd, buf, len, 0);
#endif
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bm_tls_recv(void *ctx, unsigned char *buf, size_t len) {
    bm_tls *t = ctx;
    ssize_t n = recv(t->fd, buf, len, 0);
    if (n >= 0) return (int)n; /* 0: the peer closed the connection */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ---- operations */

static bm_tls *bm_tls_open(int fd, const char *host, const char *key, bool verify, const char *ca, size_t ca_len) {
    if (!bm_tls_init()) return NULL;
    const char *off = getenv("NODE_TLS_REJECT_UNAUTHORIZED");
    if (off && strcmp(off, "0") == 0) verify = false;
    bm_tls_conf *conf = bm_tls_conf_for(verify, ca, ca_len);
    if (!conf) return NULL;
    bm_tls *t = calloc(1, sizeof *t);
    t->fd = fd;
    t->conf = conf;
    snprintf(t->key, sizeof t->key, "%s|%d|%p", key, verify, (void *)conf);
    mbedtls_ssl_init(&t->ssl);
    if (mbedtls_ssl_setup(&t->ssl, &conf->cfg) != 0) {
        mbedtls_ssl_free(&t->ssl);
        free(t);
        return NULL;
    }
    /* SNI and the name the certificate must match: an IP address literal needs no SNI */
    char name[256];
    size_t hl = strlen(host);
    if (hl >= 2 && host[0] == '[' && hl - 2 < sizeof name) { memcpy(name, host + 1, hl - 2); name[hl - 2] = 0; }
    else snprintf(name, sizeof name, "%s", host);
    mbedtls_ssl_set_hostname(&t->ssl, name);
    mbedtls_ssl_set_verify(&t->ssl, bm_tls_verify_cb, t);
    mbedtls_ssl_set_bio(&t->ssl, t, bm_tls_send, bm_tls_recv, NULL);
    bm_tls_saved *s = bm_tls_find(t->key);
    if (s) mbedtls_ssl_set_session(&t->ssl, &s->s);
    return t;
}

enum { BM_TLS_OK = 0, BM_TLS_WANT_READ = 1, BM_TLS_WANT_WRITE = 2, BM_TLS_FAILED = -1 };

static int bm_tls_handshake(bm_tls *t) {
    for (;;) {
        int rc = mbedtls_ssl_handshake(&t->ssl);
        if (rc == 0) {
            t->verified = true;
            bm_tls_save(t);
            return BM_TLS_OK;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) return BM_TLS_WANT_READ;
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) return BM_TLS_WANT_WRITE;
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) { bm_tls_save(t); continue; }
        t->last = rc;
        return BM_TLS_FAILED;
    }
}

/* > 0 bytes; 0 the connection ended; -1 wants to read, -2 to write, -3 failed. */
static long bm_tls_read(bm_tls *t, void *buf, size_t n) {
    for (;;) {
        int rc = mbedtls_ssl_read(&t->ssl, buf, n);
        if (rc > 0) return rc;
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) return -1;
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) return -2;
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) { bm_tls_save(t); continue; }
        t->last = rc;
        return -3;
    }
}

static long bm_tls_write(bm_tls *t, const void *buf, size_t n) {
    int rc = mbedtls_ssl_write(&t->ssl, buf, n);
    if (rc >= 0) return rc;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) return -1;
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) return -2;
    t->last = rc;
    return -3;
}

static size_t bm_tls_pending(bm_tls *t) {
    return mbedtls_ssl_get_bytes_avail(&t->ssl);
}

/* Why the handshake or a read failed, as Node/Bun report it: (code, message). */
static void bm_tls_why(bm_tls *t, const char *url, const char **code, char *msg, size_t n) {
    uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
    if (t->last == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != (uint32_t)-1 && flags) {
        if (flags & MBEDTLS_X509_BADCERT_EXPIRED) { *code = "CERT_HAS_EXPIRED"; snprintf(msg, n, "certificate has expired"); return; }
        if (flags & MBEDTLS_X509_BADCERT_FUTURE) { *code = "CERT_NOT_YET_VALID"; snprintf(msg, n, "certificate is not yet valid"); return; }
        if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
            if (t->self_signed) { *code = "DEPTH_ZERO_SELF_SIGNED_CERT"; snprintf(msg, n, "self signed certificate"); }
            else if (t->chain_self_signed) { *code = "SELF_SIGNED_CERT_IN_CHAIN"; snprintf(msg, n, "self signed certificate in certificate chain"); }
            else if (t->depth == 0) { *code = "UNABLE_TO_VERIFY_LEAF_SIGNATURE"; snprintf(msg, n, "unable to verify the first certificate"); }
            else { *code = "UNABLE_TO_GET_ISSUER_CERT_LOCALLY"; snprintf(msg, n, "unable to get local issuer certificate"); }
            return;
        }
        if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) {
            *code = "ERR_TLS_CERT_ALTNAME_INVALID";
            snprintf(msg, n, "ERR_TLS_CERT_ALTNAME_INVALID fetching \"%s\". For more information, pass `verbose: true` in the second argument to fetch()", url);
            return;
        }
        char info[256];
        mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
        size_t l = strlen(info);
        while (l && (info[l - 1] == '\n' || info[l - 1] == ' ')) info[--l] = 0;
        *code = "UNABLE_TO_VERIFY_LEAF_SIGNATURE";
        snprintf(msg, n, "certificate verification failed: %s", info);
        return;
    }
    char err[160];
    mbedtls_strerror(t->last, err, sizeof err);
    *code = "ERR_SSL";
    snprintf(msg, n, "TLS connection failed: %s", err);
}

static void bm_tls_close(bm_tls *t, bool notify) {
    if (notify && t->verified) mbedtls_ssl_close_notify(&t->ssl); /* best effort: the socket may be gone */
    mbedtls_ssl_free(&t->ssl);
    free(t);
}

static const bm_tls_ops bm_tls_table = {
    bm_tls_open, bm_tls_handshake, bm_tls_read, bm_tls_write, bm_tls_pending, bm_tls_why, bm_tls_close,
};

void bm_tls_install(void) {
    bm_tls_impl = &bm_tls_table;
}

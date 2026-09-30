/* tls.c — TLS for the fetch client, on the vendored BoringSSL (vendor/boringssl).
 *
 * The build compiles this file with BoringSSL into an archive that only programs calling fetch()
 * link: their generated code calls bm_tls_install(), which points the runtime's `bm_tls_impl`
 * at the operations below. The runtime never names anything here directly.
 *
 * Certificates are verified against the system's CA bundle (loaded once per configuration):
 * SSL_CERT_FILE, else the first of the usual locations; NODE_EXTRA_CA_CERTS adds more, and a
 * request's own `tls.ca` more still. NODE_TLS_REJECT_UNAUTHORIZED=0 turns verification off, as
 * in Node and Bun. Sessions are remembered per origin, so pooled and later connections resume
 * (TLS 1.3 tickets, TLS 1.2 tickets or IDs) instead of repeating the full handshake.
 *
 * Key exchange is BoringSSL's default: the post-quantum hybrid X25519+ML-KEM-768 first, as in
 * Chrome, Firefox and Node, so recorded traffic stays safe from future quantum decryption. It
 * costs ~40 us per new connection over plain X25519 (which Bun offers); pooled connections don't
 * pay it again. */

#include "barm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/base64.h>
#include <openssl/bytestring.h>
#include <openssl/err.h>
#include <openssl/mem.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

struct bm_tls {
    SSL *ssl;
    struct bm_tls_conf *conf;
    char key[256];            /* the session cache key (origin + configuration) */
    char code[64];            /* the last failure's code (ERR_SSL_...), see bm_tls_why */
};

/* A context per (verify, extra CA): few in a program. */
typedef struct bm_tls_conf {
    struct bm_tls_conf *next;
    bool verify;
    char *ca;                 /* extra PEM, or NULL */
    size_t ca_len;
    SSL_CTX *ctx;
    bool *added;              /* which roots its store has (see bm_tls_verify) */
} bm_tls_conf;

/* The trusted roots (the system bundle and NODE_EXTRA_CA_CERTS), indexed by the canonical hash of
 * their subject name and parsed only when a server's chain names one as an issuer: a program
 * that fetches from one site parses one or two of the ~130 roots, not all of them (~13 ms and
 * ~1.4 MB saved). */
typedef struct { uint8_t *der; size_t len; uint32_t subject; X509 *x; } bm_root;
static bm_root *bm_roots;
static size_t bm_nroots;

static void bm_roots_add_pem(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t cap = 1 << 16, n = 0;
    char *text = malloc(cap);
    for (size_t r; (r = fread(text + n, 1, cap - n - 1, f)) > 0;) {
        n += r;
        if (cap - n < 4096) { cap *= 2; text = realloc(text, cap); }
    }
    fclose(f);
    text[n] = 0;
    static const char begin[] = "-----BEGIN CERTIFICATE-----", end[] = "-----END CERTIFICATE-----";
    for (char *p = text; (p = strstr(p, begin)) != NULL;) {
        p += sizeof begin - 1;
        char *e = strstr(p, end);
        if (!e) break;
        /* base64 without its line breaks */
        size_t bl = 0;
        for (char *q = p; q < e; q++) if (*q != '\n' && *q != '\r' && *q != ' ' && *q != '\t') p[bl++] = *q;
        size_t max;
        if (EVP_DecodedLength(&max, bl)) {
            uint8_t *der = malloc(max);
            size_t len;
            X509_NAME *name = NULL;
            if (EVP_DecodeBase64(der, &len, max, (const uint8_t *)p, bl)) {
                /* just the subject: Certificate { tbs { [0] version, serial, algorithm, issuer,
                 * validity, subject ... } } */
                CBS cert, tbs, subject;
                CBS_init(&cert, der, len);
                if (CBS_get_asn1(&cert, &cert, CBS_ASN1_SEQUENCE) && CBS_get_asn1(&cert, &tbs, CBS_ASN1_SEQUENCE) &&
                    CBS_get_optional_asn1(&tbs, NULL, NULL, CBS_ASN1_CONTEXT_SPECIFIC | CBS_ASN1_CONSTRUCTED | 0) &&
                    CBS_get_asn1(&tbs, NULL, CBS_ASN1_INTEGER) && CBS_get_asn1(&tbs, NULL, CBS_ASN1_SEQUENCE) &&
                    CBS_get_asn1(&tbs, NULL, CBS_ASN1_SEQUENCE) && CBS_get_asn1(&tbs, NULL, CBS_ASN1_SEQUENCE) &&
                    CBS_get_asn1_element(&tbs, &subject, CBS_ASN1_SEQUENCE)) {
                    const uint8_t *in = CBS_data(&subject);
                    name = d2i_X509_NAME(NULL, &in, (long)CBS_len(&subject));
                }
            }
            if (name) {
                /* the subject's hash; only the DER is kept (parsed if a chain needs it) */
                uint32_t h = (uint32_t)X509_NAME_hash(name);
                X509_NAME_free(name);
                bm_roots = realloc(bm_roots, (bm_nroots + 1) * sizeof *bm_roots);
                bm_roots[bm_nroots++] = (bm_root){ der, len, h, NULL };
                der = NULL;
            }
            free(der);
        }
        p = e + sizeof end - 1;
    }
    free(text);
    ERR_clear_error();
}

static bool bm_tls_ready;
static const char *bm_tls_bundle;   /* the CA bundle file */
static bm_tls_conf *bm_tls_confs;
static int bm_tls_index;            /* SSL ex_data slot holding the bm_tls */

/* ---- remembered sessions (most recent per key) */

#define BM_TLS_SESSIONS 64
typedef struct { char key[256]; SSL_SESSION *s; unsigned long stamp; } bm_tls_saved;
static bm_tls_saved bm_tls_sessions[BM_TLS_SESSIONS];
static unsigned long bm_tls_clock;

static bm_tls_saved *bm_tls_find(const char *key) {
    for (int i = 0; i < BM_TLS_SESSIONS; i++)
        if (bm_tls_sessions[i].s && strcmp(bm_tls_sessions[i].key, key) == 0) return &bm_tls_sessions[i];
    return NULL;
}

/* BoringSSL hands over new sessions (after a handshake, or tickets that arrive later). */
static int bm_tls_new_session(SSL *ssl, SSL_SESSION *s) {
    bm_tls *t = SSL_get_ex_data(ssl, bm_tls_index);
    if (!t) return 0;
    bm_tls_saved *e = bm_tls_find(t->key);
    if (!e) {
        e = &bm_tls_sessions[0];
        for (int i = 0; i < BM_TLS_SESSIONS; i++) {
            if (!bm_tls_sessions[i].s) { e = &bm_tls_sessions[i]; break; }
            if (bm_tls_sessions[i].stamp < e->stamp) e = &bm_tls_sessions[i];
        }
    }
    if (e->s) SSL_SESSION_free(e->s);
    e->s = s; /* ours now (returning 1 keeps the reference) */
    snprintf(e->key, sizeof e->key, "%s", t->key);
    e->stamp = ++bm_tls_clock;
    return 1;
}

/* ---- roots */

static void bm_tls_init(void) {
    if (bm_tls_ready) return;
    bm_tls_ready = true;
    bm_tls_index = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    const char *file = getenv("SSL_CERT_FILE");
    if (file && *file) { bm_tls_bundle = file; return; }
    bm_tls_bundle = NULL;
    static const char *const bundles[] = {
        "/etc/ssl/cert.pem",                                  /* macOS, Alpine, BSDs */
        "/etc/ssl/certs/ca-certificates.crt",                 /* Debian, Ubuntu, Arch */
        "/etc/pki/tls/certs/ca-bundle.crt",                   /* Fedora, RHEL */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  /* newer RHEL */
        "/etc/ssl/ca-bundle.pem",                             /* openSUSE */
    };
    for (size_t i = 0; i < sizeof bundles / sizeof *bundles; i++)
        if (access(bundles[i], R_OK) == 0) { bm_tls_bundle = bundles[i]; return; }
}

/* The roots, indexed on first use (the first verification). */
static void bm_roots_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    if (bm_tls_bundle) bm_roots_add_pem(bm_tls_bundle);
    const char *extra = getenv("NODE_EXTRA_CA_CERTS");
    if (extra && *extra) bm_roots_add_pem(extra);
}

/* Adds to the context's store the roots that could have issued `x`. */
static void bm_tls_add_issuers(bm_tls_conf *c, X509 *x) {
    uint32_t h = (uint32_t)X509_NAME_hash(X509_get_issuer_name(x));
    X509_STORE *store = SSL_CTX_get_cert_store(c->ctx);
    for (size_t i = 0; i < bm_nroots; i++) {
        if (bm_roots[i].subject != h || c->added[i]) continue;
        if (!bm_roots[i].x) {
            const uint8_t *in = bm_roots[i].der;
            bm_roots[i].x = d2i_X509(NULL, &in, (long)bm_roots[i].len);
        }
        if (bm_roots[i].x) X509_STORE_add_cert(store, bm_roots[i].x);
        c->added[i] = true;
    }
}

/* Certificate verification: bring in the roots this chain can use, then verify as usual. */
static int bm_tls_verify(X509_STORE_CTX *sc, void *arg) {
    bm_tls_conf *c = arg;
    bm_roots_load();
    if (!c->added) c->added = calloc(bm_nroots ? bm_nroots : 1, sizeof *c->added);
    bm_tls_add_issuers(c, X509_STORE_CTX_get0_cert(sc));
    STACK_OF(X509) *chain = X509_STORE_CTX_get0_untrusted(sc);
    for (size_t i = 0; chain && i < sk_X509_num(chain); i++) bm_tls_add_issuers(c, sk_X509_value(chain, i));
    ERR_clear_error();
    return X509_verify_cert(sc);
}

/* Adds every certificate in PEM text to the store. */
static void bm_tls_add_pem(X509_STORE *store, const char *pem, size_t len) {
    BIO *bio = BIO_new_mem_buf(pem, (ptrdiff_t)len);
    if (!bio) return;
    X509 *x;
    while ((x = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        X509_STORE_add_cert(store, x);
        X509_free(x);
    }
    ERR_clear_error(); /* the read that found no more certificates */
    BIO_free(bio);
}

static bm_tls_conf *bm_tls_conf_for(bool verify, const char *ca, size_t ca_len) {
    for (bm_tls_conf *c = bm_tls_confs; c; c = c->next)
        if (c->verify == verify && c->ca_len == ca_len && (ca_len == 0 || memcmp(c->ca, ca, ca_len) == 0)) return c;
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (!ctx) return NULL;
    bm_tls_conf *c = calloc(1, sizeof *c);
    c->verify = verify;
    c->ctx = ctx;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    static const uint8_t alpn[] = "\x08http/1.1";
    SSL_CTX_set_alpn_protos(ctx, alpn, sizeof alpn - 1);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
    SSL_CTX_sess_set_new_cb(ctx, bm_tls_new_session);
    SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    if (verify) {
        /* the system's roots come in on demand; a request's own `ca` right away */
        SSL_CTX_set_cert_verify_callback(ctx, bm_tls_verify, c);
        if (ca_len) bm_tls_add_pem(SSL_CTX_get_cert_store(ctx), ca, ca_len);
        ERR_clear_error();
    }
    if (ca_len) {
        c->ca = malloc(ca_len);
        memcpy(c->ca, ca, ca_len);
        c->ca_len = ca_len;
    }
    c->next = bm_tls_confs;
    bm_tls_confs = c;
    return c;
}

/* ---- operations */

static bm_tls *bm_tls_open(int fd, const char *host, const char *key, bool verify, const char *ca, size_t ca_len) {
    bm_tls_init();
    const char *off = getenv("NODE_TLS_REJECT_UNAUTHORIZED");
    if (off && strcmp(off, "0") == 0) verify = false;
    bm_tls_conf *conf = bm_tls_conf_for(verify, ca, ca_len);
    if (!conf) return NULL;
    SSL *ssl = SSL_new(conf->ctx);
    if (!ssl) return NULL;
    bm_tls *t = calloc(1, sizeof *t);
    t->ssl = ssl;
    t->conf = conf;
    snprintf(t->key, sizeof t->key, "%s|%d|%p", key, verify, (void *)conf);
    SSL_set_ex_data(ssl, bm_tls_index, t);
    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);
    /* the name the certificate must match; SNI unless it's an IP address */
    char name[256];
    size_t hl = strlen(host);
    bool ip6 = hl >= 2 && host[0] == '[';
    if (ip6 && hl - 2 < sizeof name) { memcpy(name, host + 1, hl - 2); name[hl - 2] = 0; }
    else snprintf(name, sizeof name, "%s", host);
    X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
    if (X509_VERIFY_PARAM_set1_ip_asc(param, name) != 1) {
        SSL_set_tlsext_host_name(ssl, name);
        X509_VERIFY_PARAM_set1_host(param, name, strlen(name));
    }
    ERR_clear_error();
    bm_tls_saved *s = bm_tls_find(t->key);
    if (s) SSL_set_session(ssl, s->s);
    return t;
}

/* An SSL call's result: > 0 as is; else -1 wants read, -2 wants write, -3 failed, or 0 when
 * the connection ended (close_notify, or the peer closed the socket). */
static long bm_tls_result(bm_tls *t, int rc) {
    if (rc > 0) return rc;
    int err = SSL_get_error(t->ssl, rc);
    switch (err) {
    case SSL_ERROR_WANT_READ: return -1;
    case SSL_ERROR_WANT_WRITE: return -2;
    case SSL_ERROR_ZERO_RETURN: return 0;
    case SSL_ERROR_SYSCALL: return ERR_peek_error() == 0 && (rc == 0 || errno == 0) ? 0 : -3;
    default: return -3;
    }
}

static int bm_tls_handshake(bm_tls *t) {
    int rc = SSL_do_handshake(t->ssl);
    if (rc == 1) return 0;
    long r = bm_tls_result(t, rc);
    return r == -1 ? 1 : r == -2 ? 2 : -1;
}

static long bm_tls_read(bm_tls *t, void *buf, size_t n) {
    return bm_tls_result(t, SSL_read(t->ssl, buf, n > INT32_MAX ? INT32_MAX : (int)n));
}

static long bm_tls_write(bm_tls *t, const void *buf, size_t n) {
    return bm_tls_result(t, SSL_write(t->ssl, buf, n > INT32_MAX ? INT32_MAX : (int)n));
}

static size_t bm_tls_pending(bm_tls *t) {
    return (size_t)SSL_pending(t->ssl);
}

/* Why the handshake or a read failed, as Node and Bun report it: (code, message). */
static void bm_tls_why(bm_tls *t, const char *url, const char **code, char *msg, size_t n) {
    long vr = SSL_get_verify_result(t->ssl);
    static const char verb[] = "For more information, pass `verbose: true` in the second argument to fetch()";
    if (vr != X509_V_OK) {
        static const struct { long v; const char *code, *msg; } known[] = {
            {X509_V_ERR_CERT_HAS_EXPIRED, "CERT_HAS_EXPIRED", "certificate has expired"},
            {X509_V_ERR_CERT_NOT_YET_VALID, "CERT_NOT_YET_VALID", "certificate is not yet valid"},
            {X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT, "DEPTH_ZERO_SELF_SIGNED_CERT", "self signed certificate"},
            {X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN, "SELF_SIGNED_CERT_IN_CHAIN", "self signed certificate in certificate chain"},
            {X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY, "UNABLE_TO_GET_ISSUER_CERT_LOCALLY", "unable to get local issuer certificate"},
            {X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE, "UNABLE_TO_VERIFY_LEAF_SIGNATURE", "unable to verify the first certificate"},
            {X509_V_ERR_CERT_REVOKED, "CERT_REVOKED", "certificate revoked"},
            {X509_V_ERR_INVALID_PURPOSE, "INVALID_PURPOSE", "unsupported certificate purpose"},
            {X509_V_ERR_CERT_SIGNATURE_FAILURE, "CERT_SIGNATURE_FAILURE", "certificate signature failure"},
        };
        /* as OpenSSL (and so Node and Bun) say it: a lone certificate whose issuer is unknown */
        STACK_OF(X509) *chain = SSL_get_peer_cert_chain(t->ssl);
        if (vr == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY && chain && sk_X509_num(chain) <= 1) vr = X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE;
        if (vr == X509_V_ERR_HOSTNAME_MISMATCH || vr == X509_V_ERR_IP_ADDRESS_MISMATCH) {
            *code = "ERR_TLS_CERT_ALTNAME_INVALID";
            snprintf(msg, n, "ERR_TLS_CERT_ALTNAME_INVALID fetching \"%s\". %s", url, verb);
            return;
        }
        for (size_t i = 0; i < sizeof known / sizeof *known; i++) {
            if (known[i].v == vr) { *code = known[i].code; snprintf(msg, n, "%s", known[i].msg); return; }
        }
        *code = "CERT_VERIFY_FAILED";
        snprintf(msg, n, "%s", X509_verify_cert_error_string(vr));
        return;
    }
    /* anything else: OpenSSL's reason, as Node's ERR_SSL_<REASON> codes */
    uint32_t e = ERR_peek_last_error();
    const char *reason = e ? ERR_reason_error_string(e) : NULL;
    if (reason) {
        size_t k = (size_t)snprintf(t->code, sizeof t->code, "ERR_SSL_");
        for (const char *p = reason; *p && k + 1 < sizeof t->code; p++) t->code[k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p == ' ' ? '_' : *p);
        t->code[k] = 0;
        *code = t->code;
        snprintf(msg, n, "%s fetching \"%s\". %s", t->code, url, verb);
    } else {
        *code = "ECONNRESET";
        snprintf(msg, n, "The socket connection was closed unexpectedly. %s", verb);
    }
    ERR_clear_error();
}

static void bm_tls_close(bm_tls *t, bool notify) {
    if (notify) SSL_shutdown(t->ssl); /* best effort: the socket may be gone */
    SSL_free(t->ssl);
    ERR_clear_error();
    free(t);
}

static const bm_tls_ops bm_tls_table = {
    bm_tls_open, bm_tls_handshake, bm_tls_read, bm_tls_write, bm_tls_pending, bm_tls_why, bm_tls_close,
};

void bm_tls_install(void) {
    bm_tls_impl = &bm_tls_table;
    bm_codec = &bm_codecs;
}

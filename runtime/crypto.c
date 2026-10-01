/* crypto.c — hashes, HMAC, random bytes and key derivation on the vendored BoringSSL, for the
 * `crypto` module of npm packages (runtime/node.c wraps these for JavaScript). Compiled into
 * the same archive as tls.c; bm_crypto_install() (or bm_tls_install()) points the runtime's
 * bm_crypto here, so only programs that need it link it. Plain C: no JavaScriptCore here. */

#include "barm.h"

#include <ctype.h>
#include <string.h>

#include <openssl/digest.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <openssl/hmac.h>
#include <openssl/mem.h>
#include <openssl/rand.h>

/* A digest by Node.js's name for it: case doesn't matter, "RSA-" is dropped, "sha-256" is
 * "sha256" */
static const EVP_MD *bm_crypto_md(const char *name) {
    char n[64];
    size_t k = 0;
    if (strncasecmp(name, "rsa-", 4) == 0) name += 4;
    for (const char *p = name; *p && k + 1 < sizeof n; p++) n[k++] = (char)tolower((unsigned char)*p);
    n[k] = 0;
    /* sha-256 and the like */
    if (strncmp(n, "sha-", 4) == 0 && isdigit((unsigned char)n[4])) memmove(n + 3, n + 4, strlen(n + 4) + 1);
    if (!strcmp(n, "sha512/256")) strcpy(n, "sha512-256");
    const EVP_MD *md = EVP_get_digestbyname(n);
    if (md) return md;
    if (!strcmp(n, "sha512-256")) return EVP_sha512_256();
    if (!strcmp(n, "blake2b256")) return EVP_blake2b256();
    if (!strcmp(n, "md5-sha1")) return EVP_md5_sha1();
    return NULL;
}

static int bm_crypto_digest_size(const char *name) {
    const EVP_MD *md = bm_crypto_md(name);
    return md ? (int)EVP_MD_size(md) : -1;
}

static void *bm_crypto_hash_new(const char *name) {
    const EVP_MD *md = bm_crypto_md(name);
    if (!md) return NULL;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (!c || !EVP_DigestInit_ex(c, md, NULL)) {
        EVP_MD_CTX_free(c);
        return NULL;
    }
    return c;
}

static void *bm_crypto_hash_copy(void *h) {
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (!c || !EVP_MD_CTX_copy_ex(c, h)) {
        EVP_MD_CTX_free(c);
        return NULL;
    }
    return c;
}

static void bm_crypto_hash_update(void *h, const uint8_t *p, size_t n) { EVP_DigestUpdate(h, p, n); }

/* (out holds EVP_MAX_MD_SIZE bytes) */
static size_t bm_crypto_hash_final(void *h, uint8_t *out) {
    unsigned n = 0;
    EVP_DigestFinal_ex(h, out, &n);
    return n;
}

static void bm_crypto_hash_free(void *h) { EVP_MD_CTX_free(h); }

static void *bm_crypto_hmac_new(const char *name, const uint8_t *key, size_t n) {
    const EVP_MD *md = bm_crypto_md(name);
    if (!md) return NULL;
    HMAC_CTX *c = HMAC_CTX_new();
    if (!c || !HMAC_Init_ex(c, key, n, md, NULL)) {
        HMAC_CTX_free(c);
        return NULL;
    }
    return c;
}

static void bm_crypto_hmac_update(void *h, const uint8_t *p, size_t n) { HMAC_Update(h, p, n); }

static size_t bm_crypto_hmac_final(void *h, uint8_t *out) {
    unsigned n = 0;
    HMAC_Final(h, out, &n);
    return n;
}

static void bm_crypto_hmac_free(void *h) { HMAC_CTX_free(h); }

static void bm_crypto_random(uint8_t *out, size_t n) { RAND_bytes(out, n); }

static bool bm_crypto_pbkdf2(const char *digest, const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint32_t iter,
                             uint8_t *out, size_t outlen) {
    const EVP_MD *md = bm_crypto_md(digest);
    return md && PKCS5_PBKDF2_HMAC((const char *)pass, plen, salt, slen, iter, md, outlen, out);
}

static bool bm_crypto_scrypt(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint64_t N, uint64_t r, uint64_t p,
                             size_t maxmem, uint8_t *out, size_t outlen) {
    return EVP_PBE_scrypt((const char *)pass, plen, salt, slen, N, r, p, maxmem, out, outlen) == 1;
}

static bool bm_crypto_hkdf(const char *digest, const uint8_t *key, size_t klen, const uint8_t *salt, size_t slen, const uint8_t *info,
                           size_t ilen, uint8_t *out, size_t outlen) {
    const EVP_MD *md = bm_crypto_md(digest);
    return md && HKDF(out, outlen, md, key, klen, salt, slen, info, ilen);
}

static bool bm_crypto_equal(const uint8_t *a, const uint8_t *b, size_t n) { return CRYPTO_memcmp(a, b, n) == 0; }

const bm_crypto_ops bm_crypto_table = {
    bm_crypto_digest_size,
    bm_crypto_hash_new, bm_crypto_hash_copy, bm_crypto_hash_update, bm_crypto_hash_final, bm_crypto_hash_free,
    bm_crypto_hmac_new, bm_crypto_hmac_update, bm_crypto_hmac_final, bm_crypto_hmac_free,
    bm_crypto_random, bm_crypto_pbkdf2, bm_crypto_scrypt, bm_crypto_hkdf, bm_crypto_equal,
};

void bm_crypto_install(void) { bm_crypto = &bm_crypto_table; }

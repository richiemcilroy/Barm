'use strict';

// internalBinding('crypto'): what tls and https need to load (Barm's crypto module doesn't use
// this binding; it has its own natives). The rest isn't written yet: using it throws.

const known = {
  // (certificates load when TLS connections are made, by Barm's TLS client)
  startLoadingCertificatesOffThread() {},
  getRootCertificates: () => [],
  getBundledRootCertificates: () => [],
  getExtraCACertificates: () => [],
  getSystemCACertificates: () => [],
  getUserRootCertificates: () => [],
  resetRootCertStore() {},
  // BoringSSL's TLS 1.2 and 1.3 suites, by their OpenSSL names
  getSSLCiphers: () => [
    'tls_aes_128_gcm_sha256', 'tls_aes_256_gcm_sha384', 'tls_chacha20_poly1305_sha256', 'ecdhe-ecdsa-aes128-gcm-sha256',
    'ecdhe-rsa-aes128-gcm-sha256', 'ecdhe-ecdsa-aes256-gcm-sha384', 'ecdhe-rsa-aes256-gcm-sha384', 'ecdhe-ecdsa-chacha20-poly1305',
    'ecdhe-rsa-chacha20-poly1305', 'ecdhe-psk-chacha20-poly1305', 'ecdhe-ecdsa-aes128-sha', 'ecdhe-rsa-aes128-sha',
    'ecdhe-psk-aes128-cbc-sha', 'ecdhe-ecdsa-aes256-sha', 'ecdhe-rsa-aes256-sha', 'ecdhe-psk-aes256-cbc-sha', 'aes128-gcm-sha256',
    'aes256-gcm-sha384', 'aes128-sha', 'psk-aes128-cbc-sha', 'aes256-sha', 'psk-aes256-cbc-sha',
  ],
  getHashes: () => ['md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512', 'sha512-256'],
  getCiphers: () => [],
  getCurves: () => ['X25519', 'P-256', 'P-384', 'P-521'],
  getFipsCrypto: () => 0,
  setFipsCrypto() {},
  testFipsCrypto: () => false,
  secureHeapUsed: () => undefined,
  setEngine: () => true,
};

module.exports = new Proxy(known, {
  get(target, key) {
    if (typeof key === 'symbol' || key === 'then') return undefined;
    return target[key] ??= function notImplemented() {
      const e = new Error(`internalBinding('crypto').${String(key)} is not implemented in Barm yet`);
      e.code = 'ERR_NOT_IMPLEMENTED';
      throw e;
    };
  },
});

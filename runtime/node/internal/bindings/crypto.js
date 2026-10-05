'use strict';

// internalBinding('crypto'): what tls and https need to load (Tov's crypto module doesn't use
// this binding; it has its own natives). The rest isn't written yet: using it throws.

// A TLS context's settings (Node.js's SecureContext, over OpenSSL's SSL_CTX): recorded, for
// tls_wrap to configure Tov's TLS client with once TLS sockets are written.
class SecureContext {
  constructor() {
    this.settings = { ca: [], certs: [], keys: [], crls: [] };
  }

  init(secureProtocol, minVersion, maxVersion) {
    Object.assign(this.settings, { secureProtocol, minVersion, maxVersion });
  }

  setOptions(options) { this.settings.options = options; }
  setCiphers(ciphers) { this.settings.ciphers = ciphers; }
  setCipherSuites(suites) { this.settings.cipherSuites = suites; }
  setECDHCurve(curve) { this.settings.ecdhCurve = curve; }
  setSigalgs(sigalgs) { this.settings.sigalgs = sigalgs; }
  setDHParam(param) { this.settings.dhParam = param; }
  setKey(key, passphrase) { this.settings.keys.push({ key, passphrase }); }
  setEngineKey() {}
  setCert(cert) { this.settings.certs.push(cert); }
  addCACert(cert) { this.settings.ca.push(cert); }
  addCRL(crl) { this.settings.crls.push(crl); }
  addRootCerts() { this.settings.rootCerts = true; }
  setAllowPartialTrustChain() { this.settings.partialChain = true; }
  setSessionIdContext(id) { this.settings.sessionIdContext = id; }
  setSessionTimeout(timeout) { this.settings.sessionTimeout = timeout; }
  setTicketKeys(keys) { this.settings.ticketKeys = keys; }
  getTicketKeys() { return this.settings.ticketKeys; }
  setClientCertEngine() {}
  loadPKCS12() {
    const e = new Error('PKCS#12 isn\'t supported by Tov yet');
    e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
    throw e;
  }
  setMinProto(v) { this.settings.minVersion = v; }
  setMaxProto(v) { this.settings.maxVersion = v; }
  getMinProto() { return this.settings.minVersion; }
  getMaxProto() { return this.settings.maxVersion; }
  enableTicketKeyCallback() {}
  close() {}
}

const known = {
  SecureContext,
  // (certificates load when TLS connections are made, by Tov's TLS client)
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
      const e = new Error(`internalBinding('crypto').${String(key)} is not implemented in Tov yet`);
      e.code = 'ERR_NOT_IMPLEMENTED';
      throw e;
    };
  },
});

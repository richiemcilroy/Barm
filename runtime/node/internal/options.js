'use strict';

// internal/options: Node.js's command-line options. A Tov program has none of Node.js's
// flags: every option has its default.

const defaults = {
  __proto__: null,
  '--pending-deprecation': false,
  '--no-deprecation': false,
  '--throw-deprecation': false,
  '--trace-deprecation': false,
  '--trace-warnings': false,
  '--warnings': true,
  '--experimental-require-module': true,
  '--preserve-symlinks': false,
  '--preserve-symlinks-main': false,
  '--input-type': '',
  '--conditions': [],
  '--disable-warning': [],
  '--expose-internals': false,
  '--frozen-intrinsics': false,
  '--experimental-permission': false,
  '--permission': false,
  '--network-family-autoselection': true,
  '--network-family-autoselection-attempt-timeout': 250,
  '--max-http-header-size': 16384,
  '--insecure-http-parser': false,
  '--unhandled-rejections': 'throw',
  // (Node.js's default cipher list, DEFAULT_CIPHER_LIST_CORE)
  '--tls-cipher-list': 'TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:ECDHE-RSA-AES128-GCM-SHA256:' +
    'ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:' +
    'ECDHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA256:ECDHE-RSA-AES256-SHA384:DHE-RSA-AES256-SHA384:ECDHE-RSA-AES256-SHA256:' +
    'DHE-RSA-AES256-SHA256:HIGH:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!SRP:!CAMELLIA',
  '--tls-min-v1.0': false,
  '--tls-min-v1.1': false,
  '--tls-min-v1.2': false,
  '--tls-min-v1.3': false,
  '--tls-max-v1.2': false,
  '--tls-max-v1.3': false,
  '--trace-tls': false,
  '--use-openssl-ca': false,
  '--use-system-ca': false,
  '[has_eval_string]': false,
};

function getOptionValue(name) {
  return defaults[name];
}

module.exports = {
  getOptionValue,
  getAllowUnauthorized: () => false,
  getEmbedderOptions: () => ({ shouldNotRegisterESMLoader: true, noGlobalSearchPaths: true, hasEmbedderPreload: false }),
  refreshOptions() {},
};

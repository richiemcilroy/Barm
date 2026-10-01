'use strict';

// internal/options: Node.js's command-line options. A Barm program has none of Node.js's
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

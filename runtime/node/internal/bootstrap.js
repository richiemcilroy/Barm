'use strict';

// What Node.js gives its own lib/ modules, for the shims taken from there: `primordials` (the
// built-ins as they were before user code ran) and `internalBinding(name)`, which in Node.js
// reaches its C++ and here loads internal/bindings/<name>.js (JS, over Barm's natives).

const primordials = require('internal/primordials');

// (literal requires, so the bundler finds them; each loads on first use)
const loaders = {
  __proto__: null,
  async_context_frame: () => require('internal/bindings/async_context_frame'),
  async_wrap: () => require('internal/bindings/async_wrap'),
  blob: () => require('internal/bindings/blob'),
  buffer: () => require('internal/bindings/buffer'),
  config: () => require('internal/bindings/config'),
  constants: () => require('internal/bindings/constants'),
  encoding_binding: () => require('internal/bindings/encoding_binding'),
  errors: () => require('internal/bindings/errors'),
  fs: () => require('internal/bindings/fs'),
  messaging: () => require('internal/bindings/messaging'),
  performance: () => require('internal/bindings/performance'),
  permission: () => require('internal/bindings/permission'),
  process_methods: () => require('internal/bindings/process_methods'),
  stream_wrap: () => require('internal/bindings/stream_wrap'),
  string_decoder: () => require('internal/bindings/string_decoder'),
  symbols: () => require('internal/bindings/symbols'),
  task_queue: () => require('internal/bindings/task_queue'),
  timers: () => require('internal/bindings/timers'),
  trace_events: () => require('internal/bindings/trace_events'),
  types: () => require('internal/bindings/types'),
  url: () => require('internal/bindings/url'),
  url_pattern: () => require('internal/bindings/url_pattern'),
  util: () => require('internal/bindings/util'),
  uv: () => require('internal/bindings/uv'),
  worker: () => require('internal/bindings/worker'),
};

const bindings = { __proto__: null };
function internalBinding(name) {
  const load = loaders[name];
  if (load === undefined) throw new Error(`No such binding: ${name}`);
  return bindings[name] ??= load();
}

module.exports = { primordials, internalBinding };

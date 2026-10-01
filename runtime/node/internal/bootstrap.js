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
  block_list: () => require('internal/bindings/block_list'),
  buffer: () => require('internal/bindings/buffer'),
  cares_wrap: () => require('internal/bindings/cares_wrap'),
  config: () => require('internal/bindings/config'),
  constants: () => require('internal/bindings/constants'),
  contextify: () => require('internal/bindings/contextify'),
  credentials: () => require('internal/bindings/credentials'),
  crypto: () => require('internal/bindings/crypto'),
  encoding_binding: () => require('internal/bindings/encoding_binding'),
  errors: () => require('internal/bindings/errors'),
  fs: () => require('internal/bindings/fs'),
  fs_dir: () => require('internal/bindings/fs_dir'),
  fs_event_wrap: () => require('internal/bindings/fs_event_wrap'),
  heap_utils: () => require('internal/bindings/heap_utils'),
  http_parser: () => require('internal/bindings/http_parser'),
  internal_only_v8: () => require('internal/bindings/internal_only_v8'),
  js_stream: () => require('internal/bindings/js_stream'),
  locks: () => require('internal/bindings/locks'),
  messaging: () => require('internal/bindings/messaging'),
  os: () => require('internal/bindings/os'),
  performance: () => require('internal/bindings/performance'),
  permission: () => require('internal/bindings/permission'),
  pipe_wrap: () => require('internal/bindings/pipe_wrap'),
  process_methods: () => require('internal/bindings/process_methods'),
  process_wrap: () => require('internal/bindings/process_wrap'),
  profiler: () => require('internal/bindings/profiler'),
  serdes: () => require('internal/bindings/serdes'),
  spawn_sync: () => require('internal/bindings/spawn_sync'),
  stream_wrap: () => require('internal/bindings/stream_wrap'),
  string_decoder: () => require('internal/bindings/string_decoder'),
  symbols: () => require('internal/bindings/symbols'),
  task_queue: () => require('internal/bindings/task_queue'),
  tcp_wrap: () => require('internal/bindings/tcp_wrap'),
  timers: () => require('internal/bindings/timers'),
  tls_wrap: () => require('internal/bindings/tls_wrap'),
  trace_events: () => require('internal/bindings/trace_events'),
  tty_wrap: () => require('internal/bindings/tty_wrap'),
  types: () => require('internal/bindings/types'),
  udp_wrap: () => require('internal/bindings/udp_wrap'),
  url: () => require('internal/bindings/url'),
  url_pattern: () => require('internal/bindings/url_pattern'),
  util: () => require('internal/bindings/util'),
  uv: () => require('internal/bindings/uv'),
  v8: () => require('internal/bindings/v8'),
  worker: () => require('internal/bindings/worker'),
  zlib: () => require('internal/bindings/zlib'),
};

const bindings = { __proto__: null };
function internalBinding(name) {
  const load = loaders[name];
  if (load === undefined) throw new Error(`No such binding: ${name}`);
  return bindings[name] ??= load();
}

module.exports = { primordials, internalBinding };

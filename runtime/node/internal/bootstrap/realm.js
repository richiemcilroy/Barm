'use strict';

// internal/bootstrap/realm: which module ids are Node.js built-ins (util.inspect uses it to
// tell node_modules frames from core ones in stack traces).

const builtinIds = [
  'assert', 'assert/strict', 'async_hooks', 'buffer', 'child_process', 'cluster', 'console', 'constants',
  'crypto', 'dgram', 'diagnostics_channel', 'dns', 'dns/promises', 'domain', 'events', 'fs', 'fs/promises',
  'http', 'http2', 'https', 'inspector', 'inspector/promises', 'module', 'net', 'os', 'path', 'path/posix',
  'path/win32', 'perf_hooks', 'process', 'punycode', 'querystring', 'readline', 'readline/promises', 'repl',
  'stream', 'stream/consumers', 'stream/promises', 'stream/web', 'string_decoder', 'sys', 'timers',
  'timers/promises', 'tls', 'trace_events', 'tty', 'url', 'util', 'util/types', 'v8', 'vm', 'wasi',
  'worker_threads', 'zlib',
];
const ids = new Set(builtinIds);

const BuiltinModule = {
  exists: (id) => ids.has(id) || id.startsWith('internal/'),
  canBeRequiredByUsers: (id) => ids.has(id),
  canBeRequiredWithoutScheme: (id) => ids.has(id),
  getSchemeOnlyModuleNames: () => ['sea', 'sqlite', 'test', 'test/reporters'],
  getCanBeRequiredByUsersWithoutSchemeList: () => builtinIds.slice(),
  normalizeRequirableId: (id) => (id.startsWith('node:') ? id.slice(5) : id),
  isBuiltin: (id) => ids.has(id.startsWith('node:') ? id.slice(5) : id),
};

module.exports = { BuiltinModule, builtinIds };

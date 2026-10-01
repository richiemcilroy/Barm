'use strict';

// The natives that runtime/node.c gives a Barm program (globalThis.__barm_native), made from
// what any JavaScript host has: for a bundle run outside Barm's runtime (a bare engine, tests).
// Output goes to print() or console.log a line at a time; timers use the host's setTimeout if it
// has one (captured before Node.js's replaces it), else promise jobs.

const g = globalThis;
const hostSetTimeout = typeof g.setTimeout === 'function' ? g.setTimeout : null;
const hostPrint = typeof g.print === 'function' ? g.print : typeof g.console?.log === 'function' ? g.console.log.bind(g.console) : () => {};
const clock = typeof g.performance?.now === 'function' ? () => g.performance.now() : () => Date.now();
const start = clock();

const pending = { 1: '', 2: '' };
let onTimer = null;
let onCheck = null;
let generation = 0;
const later = (fn, ms) => (hostSetTimeout ? hostSetTimeout(fn, ms) : Promise.resolve().then(fn));

module.exports = {
  info: () => ({
    argv: ['barm'],
    execArgv: [],
    execPath: '/usr/local/bin/barm',
    pid: 1,
    ppid: 0,
    platform: 'darwin',
    arch: 'arm64',
    env: {},
    title: 'barm',
  }),
  cwd: () => '/',
  chdir() {},
  exit(code) {
    const e = new Error(`process.exit(${code})`);
    e.code = 'ERR_PROCESS_EXIT';
    throw e;
  },
  umask: () => 0o22,
  hrtime: () => (clock() - start) * 1e6,
  write(fd, data) {
    const text = typeof data === 'string' ? data : String.fromCharCode.apply(null, data);
    const lines = (pending[fd] + text).split('\n');
    pending[fd] = lines.pop();
    for (const line of lines) hostPrint(line);
    return data.length;
  },
  isatty: () => false,
  windowSize: () => [80, 24],
  memoryUsage: () => ({ rss: 0, heapTotal: 0, heapUsed: 0, external: 0, arrayBuffers: 0 }),
  cpuUsage: () => [0, 0],
  ids: () => [0, 0, 0, 0],
  kill() {},
  now: () => Math.floor(clock() - start),
  timerSetup(timer, check) {
    onTimer = timer;
    onCheck = check;
  },
  timerSchedule(ms) {
    const gen = ++generation;
    later(() => {
      if (gen === generation) onTimer();
    }, ms);
  },
  timerRef() {},
  requestCheck() {
    later(() => onCheck(), 0);
  },
  // os: what's knowable without the OS (neutral values)
  os: {
    getOSInformation: () => ['Darwin', '', '0.0.0', 'arm64'],
    getHostname: () => 'localhost',
    getHomeDirectory: () => '/',
    getUptime: () => Math.floor((clock() - start) / 1000),
    getTotalMem: () => 0,
    getFreeMem: () => 0,
    getLoadAvg: () => [0, 0, 0],
    getCPUs: () => [],
    getInterfaceAddresses: () => [],
    getUserInfo: () => ({ uid: 0, gid: 0, username: 'barm', homedir: '/', shell: null }),
    getPriority: () => 0,
    setPriority: () => 0,
    getAvailableParallelism: () => 1,
  },
};

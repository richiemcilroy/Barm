'use strict';

// internalBinding('os'): Node.js's src/node_os.cc. The natives (globalThis.__tov_native.os, from
// runtime/node.c) are named as Node.js's binding functions are and answer the same way; a
// function that fails returns undefined after filling ctx with { errno, code, message, syscall },
// which os.js turns into ERR_SYSTEM_ERROR.

const native = globalThis.__tov_native?.os ?? require('internal/bootstrap/host_fallback').os;

module.exports = {
  // -> [type, version, release, machine]
  getOSInformation: () => native.getOSInformation(),
  getHostname: (ctx) => native.getHostname(ctx),
  getHomeDirectory: (ctx) => native.getHomeDirectory(ctx),
  getUptime: (ctx) => native.getUptime(ctx),
  getTotalMem: () => native.getTotalMem(),
  getFreeMem: () => native.getFreeMem(),
  // fills a Float64Array(3)
  getLoadAvg(out) {
    const avg = native.getLoadAvg();
    out[0] = avg[0];
    out[1] = avg[1];
    out[2] = avg[2];
  },
  // -> [model, speed, user, nice, sys, idle, irq, model, ...]
  getCPUs: () => native.getCPUs(),
  // -> [name, address, netmask, family, mac, internal, scopeid, ...]
  getInterfaceAddresses: (ctx) => native.getInterfaceAddresses(ctx),
  // -> { uid, gid, username, homedir, shell }
  getUserInfo: (options, ctx) => native.getUserInfo(options?.encoding === 'buffer', ctx),
  getPriority: (pid, ctx) => native.getPriority(pid, ctx),
  setPriority: (pid, priority, ctx) => native.setPriority(pid, priority, ctx),
  getAvailableParallelism: () => native.getAvailableParallelism(),
  isBigEndian: false,
};

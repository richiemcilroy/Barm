'use strict';

// internalBinding('locks'): the Web Locks API's lock manager (Node.js's src/node_locks.cc), for
// this thread. request() grants a lock when its name's queue allows it (shared locks together,
// an exclusive one alone), calls the callback with it, and settles the returned promise with
// the callback's result once that settles, releasing the lock. steal takes a name's locks from
// their holders, whose promises reject with LOCK_STOLEN.

const LOCK_MODE_SHARED = 'shared';
const LOCK_MODE_EXCLUSIVE = 'exclusive';
const LOCK_STOLEN_ERROR = 'LOCK_STOLEN';

const held = new Map(); // name -> [lock]
const queues = new Map(); // name -> [request]

function grantable(name, mode) {
  if (queues.get(name)?.length) return false;
  const h = held.get(name);
  if (!h?.length) return true;
  return mode === LOCK_MODE_SHARED && h.every((l) => l.mode === LOCK_MODE_SHARED);
}

function release(lock) {
  const h = held.get(lock.name);
  if (!h) return;
  const i = h.indexOf(lock);
  if (i < 0) return;
  h.splice(i, 1);
  if (h.length === 0) held.delete(lock.name);
  process(lock.name);
}

function grant(req) {
  const lock = { name: req.name, mode: req.mode, clientId: req.clientId, stolen: false, reject: req.reject };
  if (!held.has(req.name)) held.set(req.name, []);
  held.get(req.name).push(lock);
  let result;
  try {
    result = Promise.resolve(req.callback({ name: req.name, mode: req.mode }));
  } catch (e) {
    result = Promise.reject(e);
  }
  result.then((v) => {
    if (lock.stolen) return;
    release(lock);
    req.resolve(v);
  }, (e) => {
    if (lock.stolen) return;
    release(lock);
    req.reject(e);
  });
}

// grants what the name's queue allows, in order
function process(name) {
  const q = queues.get(name);
  while (q?.length) {
    const req = q[0];
    const h = held.get(name);
    const ok = !h?.length || (req.mode === LOCK_MODE_SHARED && h.every((l) => l.mode === LOCK_MODE_SHARED));
    if (!ok) break;
    q.shift();
    grant(req);
  }
  if (q && q.length === 0) queues.delete(name);
}

function request(name, clientId, mode, steal, ifAvailable, callback) {
  return new Promise((resolve, reject) => {
    const req = { name, clientId, mode, callback, resolve, reject };
    if (steal) {
      for (const lock of held.get(name) ?? []) {
        lock.stolen = true;
        lock.reject(new Error(LOCK_STOLEN_ERROR));
      }
      held.delete(name);
      grant(req);
      return;
    }
    if (grantable(name, mode)) {
      grant(req);
      return;
    }
    if (ifAvailable) {
      try {
        Promise.resolve(callback(null)).then(resolve, reject);
      } catch (e) {
        reject(e);
      }
      return;
    }
    if (!queues.has(name)) queues.set(name, []);
    queues.get(name).push(req);
  });
}

function query() {
  const out = { held: [], pending: [] };
  for (const list of held.values()) for (const l of list) out.held.push({ name: l.name, mode: l.mode, clientId: l.clientId });
  for (const list of queues.values()) for (const r of list) out.pending.push({ name: r.name, mode: r.mode, clientId: r.clientId });
  return Promise.resolve(out);
}

module.exports = { request, query, LOCK_MODE_SHARED, LOCK_MODE_EXCLUSIVE, LOCK_STOLEN_ERROR };

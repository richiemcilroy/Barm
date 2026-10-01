// The Web Locks API (worker_threads' locks): exclusive and shared locks, ifAvailable, steal, query
const { locks } = require('worker_threads');
const log = [];

(async () => {
  const a = locks.request('r', async (lock) => {
    log.push(`a got ${lock.name} ${lock.mode}`);
    await null;
    await null;
    log.push('a done');
    return 'A';
  });
  const b = locks.request('r', async () => {
    log.push('b got');
    return 'B';
  });
  const c = locks.request('r', { ifAvailable: true }, (lock) => {
    log.push(`c ${lock}`);
    return 'C';
  });
  const s1 = locks.request('s', { mode: 'shared' }, async (l) => { log.push(`s1 ${l.mode}`); await null; return 1; });
  const s2 = locks.request('s', { mode: 'shared' }, async (l) => { log.push(`s2 ${l.mode}`); return 2; });
  const q = await locks.query();
  log.push(`query held ${q.held.map((h) => `${h.name}:${h.mode}`).sort().join(',')} pending ${q.pending.map((h) => h.name).join(',')}`);
  log.push(`results ${await a} ${await b} ${await c} ${await s1} ${await s2}`);
  const victim = locks.request('t', () => new Promise(() => {}));
  await null;
  const thief = locks.request('t', { steal: true }, () => 'stolen');
  try {
    await victim;
  } catch (e) {
    log.push(`victim ${e.name} ${e.message}`);
  }
  log.push(`thief ${await thief}`);
  try {
    await locks.request('-x', () => {});
  } catch (e) {
    log.push(`hyphen ${e.name}`);
  }
  console.log(log.join('\n'));
})();

// perf_hooks, the performance global and the constants module
const { performance, PerformanceObserver, monitorEventLoopDelay } = require('perf_hooks');
const constants = require('constants');
const t0 = performance.now();
let x = 0;
for (let i = 0; i < 1e5; i++) x += i;
const t1 = performance.now();
console.log('now', typeof t0, t1 >= t0, typeof performance.timeOrigin, performance.timeOrigin > 1e12);
performance.mark('a');
performance.mark('b');
const m = performance.measure('a-b', 'a', 'b');
console.log('measure', m.name, m.entryType, m.duration >= 0, performance.getEntriesByType('mark').length);
performance.clearMarks();
console.log('cleared', performance.getEntriesByType('mark').length);
console.log('observer', typeof PerformanceObserver, typeof monitorEventLoopDelay);
console.log('global', globalThis.performance === performance, typeof performance.toJSON());
console.log('constants', constants.O_RDONLY, constants.ENOENT, constants.SIGINT, typeof constants.S_IFMT);

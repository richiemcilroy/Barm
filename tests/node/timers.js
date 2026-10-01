// timers: the orderings Node.js guarantees (ticks, then promises; inside a timer callback an
// immediate before a 0 ms timeout), clearing, refs, intervals, timers/promises
const log = [];
process.nextTick(() => log.push("tick"));
Promise.resolve().then(() => log.push("promise"));
queueMicrotask(() => log.push("microtask"));
const cleared = setTimeout(() => log.push("cleared"), 1);
clearTimeout(cleared);
const ref = setTimeout(() => {}, 1);
console.log("timer object", typeof ref.ref, typeof ref.unref, ref.hasRef(), typeof ref[Symbol.toPrimitive]);
ref.unref();
console.log("unref", ref.hasRef());
setTimeout(() => {
  log.push("timeout");
  setTimeout(() => log.push("timeout-in-timeout"), 0);
  setImmediate(() => {
    log.push("immediate-in-timeout");
    process.nextTick(() => log.push("tick-in-immediate"));
  });
  let n = 0;
  const iv = setInterval(() => {
    log.push("interval" + ++n);
    if (n === 3) {
      clearInterval(iv);
      setTimeout(async () => {
        const { setTimeout: sleep, setImmediate: imm } = require("timers/promises");
        log.push(await sleep(2, "slept"));
        log.push(await imm("imm-value"));
        console.log(log.join(" "));
      }, 0);
    }
  }, 5);
}, 1);

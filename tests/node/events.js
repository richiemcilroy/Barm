// events: EventEmitter, once, errors, listener bookkeeping
const EventEmitter = require("events");
const { once, on } = require("events");
const e = new EventEmitter();
const log = [];
e.on("a", (x) => log.push("on1:" + x));
e.prependListener("a", (x) => log.push("pre:" + x));
e.once("a", (x) => log.push("once:" + x));
e.emit("a", 1);
e.emit("a", 2);
console.log(log.join(" "), e.listenerCount("a"), e.eventNames().join(","));
e.off("a", e.listeners("a")[0]);
console.log("after off", e.listenerCount("a"), e.rawListeners("a").length);
try { e.emit("error", new Error("boom")); } catch (err) { console.log("error thrown:", err.message); }
try { e.emit("error", "str"); } catch (err) { console.log(err.code, err.message); }
e.setMaxListeners(1);
console.log("max", e.getMaxListeners(), EventEmitter.defaultMaxListeners);
class Sub extends EventEmitter {}
const s = new Sub();
console.log(s instanceof EventEmitter, typeof EventEmitter.EventEmitter, EventEmitter.EventEmitter === EventEmitter);
s.emit("newListener");
s.on("newListener", (n) => console.log("newListener", n));
s.on("x", () => {});
s.removeAllListeners("x");
console.log("x count", s.listenerCount("x"));
(async () => {
  const t = new EventEmitter();
  setTimeout(() => t.emit("ready", 42, "b"), 1);
  const args = await once(t, "ready");
  console.log("once resolved", JSON.stringify(args));
  const u = new EventEmitter();
  setTimeout(() => u.emit("error", new Error("bad")), 1);
  try { await once(u, "never"); } catch (err) { console.log("once rejected", err.message); }
})();

// the globals npm code finds without importing anything
const names = ["process", "global", "Buffer", "atob", "btoa", "queueMicrotask", "structuredClone", "TextEncoder", "TextDecoder",
  "URL", "URLSearchParams", "AbortController", "AbortSignal", "Event", "EventTarget", "CustomEvent", "DOMException",
  "MessageChannel", "MessagePort", "BroadcastChannel", "ReadableStream", "WritableStream", "TransformStream",
  "ByteLengthQueuingStrategy", "CountQueuingStrategy", "TextEncoderStream", "TextDecoderStream", "CompressionStream",
  "DecompressionStream", "Blob", "File", "setTimeout", "clearTimeout", "setInterval", "clearInterval", "setImmediate", "clearImmediate"];
console.log(names.map((n) => `${n}:${typeof globalThis[n]}`).join(" "));
console.log(global === globalThis, typeof process.nextTick, process.version, typeof process.versions.node, process.release.name, typeof process.env, Array.isArray(process.argv), typeof process.hrtime.bigint());
console.log(structuredClone({ a: [1, new Map([[1, 2]])], d: new Date(0) }).a[1].get(1), btoa("hi"), new TextDecoder().decode(new TextEncoder().encode("✓")));
const ac = new AbortController();
ac.signal.addEventListener("abort", () => console.log("aborted", ac.signal.reason.name));
ac.abort();
const et = new EventTarget();
et.addEventListener("x", (e) => console.log("event", e.type, e instanceof Event));
et.dispatchEvent(new Event("x"));
console.log(new DOMException("m", "AbortError").code, typeof new CustomEvent("c", { detail: 1 }).detail);
const { port1, port2 } = new MessageChannel();
port2.onmessage = (e) => { console.log("message", JSON.stringify(e.data)); port1.close(); port2.close(); };
port1.postMessage({ hi: [1, 2] });

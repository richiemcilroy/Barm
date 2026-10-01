// stream/web: ReadableStream, WritableStream, TransformStream, TextEncoderStream-free pipes
const { ReadableStream, WritableStream, TransformStream, ByteLengthQueuingStrategy, CountQueuingStrategy } = require("stream/web");
(async () => {
  const rs = new ReadableStream({ start(c) { c.enqueue("a"); c.enqueue("b"); c.close(); } });
  const upper = new TransformStream({ transform(chunk, c) { c.enqueue(chunk.toUpperCase()); } });
  const got = [];
  await rs.pipeThrough(upper).pipeTo(new WritableStream({ write(c) { got.push(c); } }));
  console.log("pipe", got.join(""));
  const r2 = new ReadableStream({ pull(c) { c.enqueue(1); c.close(); } });
  const reader = r2.getReader();
  console.log("read", JSON.stringify(await reader.read()), JSON.stringify(await reader.read()), r2.locked);
  const [t1, t2] = new ReadableStream({ start(c) { c.enqueue("x"); c.close(); } }).tee();
  for await (const v of t1) console.log("tee1", v);
  for await (const v of t2) console.log("tee2", v);
  const bytes = new ReadableStream({ type: "bytes", start(c) { c.enqueue(new Uint8Array([1, 2, 3])); c.close(); } });
  const br = bytes.getReader({ mode: "byob" });
  const res = await br.read(new Uint8Array(8));
  console.log("byob", res.value.length, Array.from(res.value).join(","));
  console.log("strategies", new ByteLengthQueuingStrategy({ highWaterMark: 16 }).highWaterMark, new CountQueuingStrategy({ highWaterMark: 2 }).size());
  const errs = new ReadableStream({ start(c) { c.error(new Error("bad")); } });
  try { await errs.getReader().read(); } catch (e) { console.log("error", e.message); }
  console.log(Object.prototype.toString.call(rs), typeof ReadableStream.from);
})();

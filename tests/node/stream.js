// stream: Readable, Writable, Transform, PassThrough, Duplex, pipeline, finished, async iteration
const { Readable, Writable, Transform, PassThrough, pipeline, finished, Duplex } = require("stream");
const { pipeline: pipelineP } = require("stream/promises");
const upper = new Transform({ transform(chunk, enc, cb) { cb(null, chunk.toString().toUpperCase()); } });
const out = [];
const sink = new Writable({ write(chunk, enc, cb) { out.push(chunk.toString()); cb(); } });
pipeline(Readable.from(["a", "b", "c"]), upper, sink, (err) => {
  console.log("pipeline", err ?? "ok", out.join(""));
  const pt = new PassThrough();
  pt.end("pass");
  pt.on("data", (d) => console.log("passthrough", d.toString()));
  finished(pt, (e) => console.log("finished", e ?? "ok"));
});
(async () => {
  const chunks = [];
  for await (const c of Readable.from([1, 2, 3])) chunks.push(c);
  console.log("async iter", chunks.join(","));
  const r = new Readable({ read() {} });
  r.push("x"); r.push(null);
  r.setEncoding("utf8");
  r.on("data", (d) => console.log("readable data", d));
  await pipelineP(Readable.from(["p", "q"]), new Writable({ write(c, e, cb) { console.log("promise pipeline", c.toString()); cb(); } }));
  const err = new Readable({ read() { this.destroy(new Error("boom")); } });
  err.on("error", (e) => console.log("error event", e.message));
  err.resume();
  const d = new Duplex({ read() { this.push("dup"); this.push(null); }, write(c, e, cb) { console.log("duplex write", c.toString()); cb(); } });
  d.on("data", (c) => console.log("duplex read", c.toString()));
  d.end("w");
  const objs = Readable.from([{ a: 1 }]);
  console.log("objectMode", objs.readableObjectMode);
  const mapped = await Readable.from([1, 2, 3]).map((x) => x * 2).toArray();
  console.log("map", mapped.join(","));
})();

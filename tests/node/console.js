// console (Node.js's, over process.stdout and stderr): what reaches stdout
const c = globalThis.__node_console ?? require("console");
const out = [];
const { Console } = require("console");
const { Writable } = require("stream");
const sink = new Writable({ write(chunk, enc, cb) { out.push(chunk.toString()); cb(); } });
const con = new Console({ stdout: sink, stderr: sink, colorMode: false });
con.log("a %s %d %o", "str", 42, { x: [1] });
con.info({ deep: { a: { b: { c: {} } } } });
con.table([{ a: 1, b: "x" }, { a: 2, b: "y" }]);
con.group("group"); con.log("inside"); con.groupEnd(); con.log("outside");
con.count(); con.count(); con.count("k"); con.countReset();
con.assert(1 === 2, "assert %s", "msg");
con.dir({ a: 1 }, { depth: 0 });
con.error(new Error("e").message);
console.log(JSON.stringify(out.join("")));

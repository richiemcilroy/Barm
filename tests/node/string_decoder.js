// string_decoder: characters split across chunks, invalid bytes, flush, every encoding
const { StringDecoder } = require("string_decoder");
const bytes = Buffer.from("aé€😀b");
for (const step of [1, 2, 3]) {
  const d = new StringDecoder("utf8");
  let out = "";
  for (let i = 0; i < bytes.length; i += step) out += "[" + d.write(bytes.subarray(i, i + step)) + "]";
  console.log("utf8 step", step, out + "[" + d.end() + "]");
}
const bad = new StringDecoder("utf8");
console.log("bad", JSON.stringify(bad.write(Buffer.from([0xe2, 0x82]))), JSON.stringify(bad.write(Buffer.from([0x41]))), JSON.stringify(bad.end(Buffer.from([0xf0, 0x9f]))));
const u = new StringDecoder("utf16le");
const ub = Buffer.from("a😀b", "utf16le");
let us = "";
for (let i = 0; i < ub.length; i++) us += "[" + u.write(ub.subarray(i, i + 1)) + "]";
console.log("utf16le", us, JSON.stringify(u.end()));
const b64 = new StringDecoder("base64");
console.log("base64", b64.write(Buffer.from("he")), b64.write(Buffer.from("llo")), b64.end());
const hex = new StringDecoder("hex");
console.log("hex", hex.write(Buffer.from([1, 255])), hex.end(), new StringDecoder("latin1").write(Buffer.from([0xe9])), new StringDecoder().encoding);
const s = new StringDecoder("utf8"); s.write(Buffer.from([0xe2]));
console.log("state", s.lastNeed, s.lastTotal, JSON.stringify(s.lastChar.subarray(0, 1)));
try { new StringDecoder("nope"); } catch (e) { console.log(e.code, e.message); }

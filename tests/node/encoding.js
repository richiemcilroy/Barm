// TextEncoder / TextDecoder (util's, and the stream versions)
const { TextEncoder, TextDecoder } = require("util");
const enc = new TextEncoder();
console.log(enc.encoding, Array.from(enc.encode("hé😀")).join(","), Array.from(enc.encode()).length);
const dest = new Uint8Array(5);
console.log(JSON.stringify(enc.encodeInto("aé😀b", dest)), Array.from(dest).join(","));
const dec = new TextDecoder();
console.log(dec.encoding, dec.fatal, dec.ignoreBOM, JSON.stringify(dec.decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x68, 0xff, 0x69]))));
console.log(JSON.stringify(new TextDecoder("utf-8", { ignoreBOM: true }).decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x41]))));
try { new TextDecoder("utf-8", { fatal: true }).decode(new Uint8Array([0xc3])); } catch (e) { console.log(e.name, e.code, e.message); }
const s = new TextDecoder();
console.log(JSON.stringify(s.decode(new Uint8Array([0xe2, 0x82]), { stream: true }) + s.decode(new Uint8Array([0xac]))));
console.log(new TextDecoder("utf-16le").decode(new Uint8Array([0x68, 0, 0x69, 0])), new TextDecoder("latin1").encoding, new TextDecoder("latin1").decode(new Uint8Array([0xe9])));
try { new TextDecoder("nope"); } catch (e) { console.log(e.name, e.code); }

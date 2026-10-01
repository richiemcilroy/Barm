// buffer: encodings, invalid input, writes, fill, search, compare, numbers
const { Buffer, kMaxLength, constants, atob, btoa, isUtf8, isAscii } = require("buffer");
const show = (b) => Array.from(b).join(",");
const s = "héllo wörld ✓ 😀";
for (const enc of ["utf8", "utf16le", "latin1", "ascii", "hex", "base64", "base64url"]) {
  const b = Buffer.from(s, enc === "hex" ? "utf8" : enc);
  console.log(enc, b.length, b.toString(enc), Buffer.byteLength(s, enc));
}
console.log("bad utf8", JSON.stringify(Buffer.from([0x61, 0xff, 0xe2, 0x82, 0x62, 0xf0, 0x9f, 0x98, 0xed, 0xa0, 0x80, 0x63]).toString()));
console.log("lone surrogate", show(Buffer.from("a\ud800b")));
console.log("hex odd/bad", show(Buffer.from("abcz12", "hex")), show(Buffer.from("abc", "hex")));
console.log("base64 forgiving", show(Buffer.from("aGV sbG8=\nd29y", "base64")), show(Buffer.from("-_-_", "base64")), show(Buffer.from("+/+/", "base64url")));
const w = Buffer.alloc(8);
console.log("write", w.write("✓✓✓"), show(w), w.write("ab", 6, "latin1"), w.write("zz", 7), show(w));
console.log("fill", show(Buffer.alloc(7).fill("ab")), show(Buffer.alloc(5, "€")), show(Buffer.alloc(4).fill(257)), show(Buffer.alloc(6).fill("aGk=", "base64")));
try { Buffer.alloc(3).fill(""); console.log("empty fill ok"); } catch (e) { console.log(e.code); }
try { Buffer.alloc(3).fill("zz", "hex"); } catch (e) { console.log(e.code, e.message); }
const h = Buffer.from("abcabcabc");
console.log("indexOf", h.indexOf("bc"), h.indexOf("bc", 2), h.lastIndexOf("bc"), h.indexOf("x"), h.indexOf(99), h.indexOf(Buffer.from("ca")), h.includes("cab"), h.indexOf("", 4), h.lastIndexOf("a", -4), h.indexOf("b", -100));
console.log("ucs2 indexOf", Buffer.from("abcd", "utf16le").indexOf("c", 0, "utf16le"));
console.log("compare", Buffer.compare(Buffer.from("a"), Buffer.from("b")), Buffer.from("abc").compare(Buffer.from("abd"), 0, 2, 0, 2), Buffer.from("abc").equals(Buffer.from("abc")));
const n = Buffer.alloc(16);
n.writeUInt32LE(0xdeadbeef, 0); n.writeInt16BE(-2, 4); n.writeDoubleLE(Math.PI, 8);
console.log("numbers", n.readUInt32LE(0).toString(16), n.readInt16BE(4), n.readDoubleLE(8), n.readBigUInt64LE(8), show(n.subarray(0, 6)));
console.log("swap", show(Buffer.from([1, 2, 3, 4]).swap16()), show(Buffer.from([1, 2, 3, 4, 5, 6, 7, 8]).swap64()));
console.log("concat/slice", Buffer.concat([Buffer.from("ab"), Buffer.from("cd")], 3).toString(), Buffer.from("hello").slice(-3, -1).toString(), Buffer.from("hello").toString("utf8", 1, 3));
console.log("copy", Buffer.from("hello").copy(w, 1, 1, 4), show(w));
console.log("json", JSON.stringify(Buffer.from("hi")), Buffer.isBuffer(Buffer.from("x")), Buffer.isEncoding("UTF-8"), Buffer.isEncoding("nope"));
console.log("atob/btoa", btoa("hello"), atob("aGVsbG8="), atob(" aGVs bG8 "));
for (const bad of ["a", "a$b="]) { try { atob(bad); } catch (e) { console.log("atob", JSON.stringify(bad), e.name, e.message); } }
console.log("isUtf8", isUtf8(Buffer.from("✓")), isUtf8(Buffer.from([0xc3])), isAscii(Buffer.from("abc")), isAscii(Buffer.from("é")));
console.log("limits", kMaxLength, constants.MAX_STRING_LENGTH);
console.log("inspect", require("util").inspect(Buffer.from("hello world")));
try { Buffer.from({}); } catch (e) { console.log(e.code, e.message); }

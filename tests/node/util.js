// util: inspect, format, promisify, inherits, types, deprecate, parseArgs, styleText, isDeepStrictEqual
const util = require("util");
class Point { constructor() { this.x = 1; this.y = [1, 2, { z: "s" }]; } }
const circ = { a: 1 }; circ.self = circ;
const vals = [
  1, -0, 1.5e300, 12345678901234567890n, "str'q", Symbol("sym"), undefined, null, true,
  [1, , 3], { a: { b: { c: { d: 1 } } } }, new Point(), circ, new Map([[1, { a: 2 }], ["k", [3]]]), new Set([1, "two"]),
  new Date(0), /re+g/gi, new Error("msg").message, function named() {}, () => {}, class K {}, async function af() {},
  new Uint8Array([1, 2, 3]), new ArrayBuffer(4), [new Array(120).fill(7)], "x".repeat(200), Object.create(null),
  { [Symbol("k")]: 1, "quoted-key": 2, valid_id: 3 }, [undefined, null], new WeakMap(), new Promise(() => {}),
  Object.assign(Object.create({ inherited: 1 }), { own: 2 }), new (class Foo extends Map {})(), { f() {}, get g() { return 1; } },
  new Number(3), new String("boxed"), Object(Symbol("boxed")),
];
for (const v of vals) console.log(util.inspect(v));
console.log(util.inspect({ a: [1, 2, [3, [4, [5]]]] }, { depth: 0 }), util.inspect({ a: 1, b: "x" }, { compact: false }));
console.log(util.inspect("colors", { colors: true }), util.inspect([1, "a", null], { colors: true }));
console.log(util.format("%s %d %i %f %j %o %O %% %c", "s", 42.5, 42.5, "3.5", { a: 1 }, [1], { b: 2 }, "css"), util.format("a", 1, { b: 2 }), util.format(1, "%s"));
const p = util.promisify((x, cb) => cb(null, x * 2));
p(21).then((v) => console.log("promisify", v));
util.promisify((cb) => cb(new Error("no")))().catch((e) => console.log("promisify err", e.message));
function A() {} function B() {} util.inherits(B, A);
console.log("inherits", new B() instanceof A, B.super_ === A);
console.log("types", util.types.isPromise(Promise.resolve()), util.types.isRegExp(/a/), util.types.isDate(new Date()), util.types.isUint8Array(new Uint8Array()), util.types.isAsyncFunction(async () => {}), util.types.isMap(new Map()), util.types.isSet({}), util.types.isGeneratorFunction(function* () {}), util.types.isBoxedPrimitive(new Number(1)), util.types.isNativeError(new TypeError()));
console.log("isDeepStrictEqual", util.isDeepStrictEqual({ a: [1, { b: 2 }] }, { a: [1, { b: 2 }] }), util.isDeepStrictEqual([1], ["1"]), util.isDeepStrictEqual(new Set([1, 2]), new Set([2, 1])));
console.log("parseArgs", JSON.stringify(util.parseArgs({ args: ["-f", "--bar", "b", "pos"], options: { foo: { type: "boolean", short: "f" }, bar: { type: "string" } }, allowPositionals: true })));
console.log("styleText", JSON.stringify(util.styleText("red", "hi", { validateStream: false })), JSON.stringify(util.stripVTControlCharacters("\u001b[31mhi\u001b[39m")));
console.log("misc", typeof util.deprecate(() => 1, "dep"), util.toUSVString("a\ud800"), typeof util.debuglog("x"), util.inspect.custom.toString());
console.log("custom", util.inspect({ [util.inspect.custom]() { return "CUSTOM"; } }), util.inspect(new (class Q { [util.inspect.custom](d, o) { return `Q<${d}>`; } })()));

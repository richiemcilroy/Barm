// assert: passing checks, and the AssertionError for failing ones
const assert = require("assert");
assert(true); assert.ok(1); assert.equal(1, "1"); assert.strictEqual(1, 1); assert.deepStrictEqual({ a: [1, { b: 2 }] }, { a: [1, { b: 2 }] });
assert.notStrictEqual(1, 2); assert.deepEqual([1], ["1"]); assert.throws(() => { throw new TypeError("x"); }, TypeError); assert.match("abc", /b/);
const fails = [
  () => assert.strictEqual(1, 2),
  () => assert.strictEqual("hello world", "hello there"),
  () => assert.deepStrictEqual({ a: 1, b: [1, 2] }, { a: 1, b: [1, 3] }),
  () => assert.ok(0),
  () => assert(false, "custom message"),
  () => assert.notStrictEqual(1, 1),
  () => assert.throws(() => {}),
  () => assert.fail("boom"),
  () => assert.match("abc", /x/),
  () => assert.equal(null, undefined, undefined),
  () => assert.strictEqual(NaN, 0),
];
for (const f of fails) {
  try { f(); console.log("no throw"); } catch (e) {
    console.log(e.name, e.code, JSON.stringify(e.operator), e.generatedMessage, "|", e.message.replace(/\n/g, "\\n"));
  }
}
const strict = require("assert/strict");
try { strict.equal(1, "1"); } catch (e) { console.log("strict", e.code); }
(async () => {
  await assert.rejects(Promise.reject(new Error("r")), /r/);
  try { await assert.doesNotReject(Promise.reject(new Error("dr"))); } catch (e) { console.log("doesNotReject", e.code, e.message.split("\n")[0]); }
})();

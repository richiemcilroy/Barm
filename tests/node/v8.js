// v8: serialize and deserialize in V8's wire format (byte for byte), heap statistics' shape
const v8 = require('v8');
const hex = (v) => v8.serialize(v).toString('hex');
const shared = { s: 1 };
const ab = new ArrayBuffer(8);
const err = new RangeError('bad', { cause: 'why' });
err.stack = 'stack text';
const values = [
  undefined, null, true, false, 0, -1, 1, 2 ** 31, -(2 ** 31), 1.5, -0, NaN, Infinity, 10n, -(2n ** 70n), 0n,
  '', 'abc', 'é', '中文', 'a中', 'x'.repeat(300), [1, 2], [1, , 3], [], { a: 1, b: { c: [1] } }, { 0: 'z', x: 1, 10: 2 },
  new Date(0), /x/gi, /y/msuy, new Map([[1, 2], ['k', { v: 1 }]]), new Set(['x', 1]), Buffer.from('hi'),
  new Float64Array([1, 2]), new Uint16Array([1, 65535]), new DataView(new ArrayBuffer(3)), ab, new Number(3),
  new String('w'), new Boolean(false), Object(5n), err, new TypeError('t'), [shared, shared], { a: ab, b: ab },
  Object.assign([1, 2], { extra: true }), Object.create(null),
];
for (const v of values) {
  if (v instanceof Error) {
    // (stacks differ between engines)
    const copy = v8.deserialize(v8.serialize(v));
    console.log('error', copy.constructor.name, copy.message, copy.cause, copy.stack === v.stack);
    continue;
  }
  const h = hex(v);
  const back = v8.deserialize(v8.serialize(v));
  console.log(h, require('util').inspect(back, { depth: 4, breakLength: 200 }));
}
const plain = new v8.Serializer();
plain.writeHeader();
const buf = new ArrayBuffer(4);
plain.writeValue([new Uint8Array(buf), new Uint16Array(buf, 2, 1)]);
plain.writeUint32(7);
plain.writeUint64(1, 2);
plain.writeDouble(0.5);
plain.writeRawBytes(Buffer.from('ab'));
const out = plain.releaseBuffer();
console.log('plain', out.toString('hex'));
const d = new v8.Deserializer(out);
d.readHeader();
const views = d.readValue();
console.log('plain read', views[0].constructor.name, views[1].constructor.name, views[0].buffer === views[1].buffer, d.readUint32(), d.readUint64(), d.readDouble(), d.readRawBytes(2).toString(), d.getWireFormatVersion());
for (const bad of [() => 1, Symbol('s'), new WeakMap(), Promise.resolve()]) {
  try {
    v8.serialize(bad);
  } catch (e) {
    console.log('unclonable', e.name, e.message);
  }
}
for (const b of [[1, 2, 3], [0xff, 15, 0x6f], [0xff, 99]]) {
  try {
    v8.deserialize(Buffer.from(b));
  } catch (e) {
    console.log('undeserializable', e.message);
  }
}
const stats = v8.getHeapStatistics();
console.log('heap', Object.keys(stats).join(','), typeof stats.used_heap_size, stats.heap_size_limit > 0);
console.log('spaces', Array.isArray(v8.getHeapSpaceStatistics()), Object.keys(v8.getHeapCodeStatistics()).join(','));
console.log('misc', typeof v8.cachedDataVersionTag(), typeof v8.setFlagsFromString, typeof v8.isStringOneByteRepresentation);

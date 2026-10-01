// querystring: parse, stringify, escape, unescape, options
const qs = require("querystring");
console.log(JSON.stringify(qs.parse("a=1&b=two+words&a=3&c=%E2%9C%93&d&=e&f=%zz")));
console.log(qs.stringify({ a: [1, 2], b: "x y", c: "✓", d: true, e: null, f: undefined, g: "" }));
console.log(qs.stringify({ a: 1 }, ";", ":"), JSON.stringify(qs.parse("a:1;b:2", ";", ":")), JSON.stringify(qs.parse("a=1&b=2&c=3", null, null, { maxKeys: 2 })));
console.log(qs.escape("a b&c/✓"), qs.unescape("a%20b%26%zz"), qs.encode === qs.stringify, qs.decode === qs.parse);

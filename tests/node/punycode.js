// punycode (deprecated in Node.js, still required by packages)
const punycode = require("punycode");
console.log(punycode.encode("mañana"), punycode.decode("maana-pta"), punycode.toASCII("mañana.com"), punycode.toUnicode("xn--maana-pta.com"), punycode.ucs2.decode("😀").join(","), punycode.version);

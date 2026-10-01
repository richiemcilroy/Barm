// path: posix and win32, against Node.js's own answers
const path = require("path");
const cases = [
  ["join", ["/a/b", "../c", "./d"]], ["join", ["a", "", "b/"]], ["join", []],
  ["resolve", ["/a/b", "./c"]], ["resolve", ["/x", "/y", "z/.."]],
  ["normalize", ["/a//b/../c/./d/"]], ["normalize", [""]], ["normalize", ["./"]],
  ["dirname", ["/a/b/c.txt"]], ["dirname", ["a"]], ["dirname", ["/"]],
  ["basename", ["/a/b/c.txt"]], ["basename", ["/a/b/c.txt", ".txt"]], ["basename", ["/a/b/"]],
  ["extname", ["index.html"]], ["extname", [".bashrc"]], ["extname", ["a.b.c"]], ["extname", ["a."]],
  ["isAbsolute", ["/a"]], ["isAbsolute", ["a"]],
  ["relative", ["/data/orandea/test/aaa", "/data/orandea/impl/bbb"]], ["relative", ["/a", "/a"]],
  ["parse", ["/home/user/dir/file.txt"]], ["format", [{ dir: "/home/user/dir", base: "file.txt" }]],
  ["toNamespacedPath", ["/a/b"]],
];
for (const [fn, args] of cases) console.log(`posix.${fn}(${JSON.stringify(args).slice(1, -1)}) = ${JSON.stringify(path.posix[fn](...args))}`);
for (const [fn, args] of [["join", ["C:\\a", "..\\b", "c"]], ["resolve", ["C:\\a", "b"]], ["normalize", ["C:/a//b\\..\\c"]], ["parse", ["C:\\path\\dir\\file.txt"]], ["relative", ["C:\\a\\b", "C:\\a\\c\\d"]], ["isAbsolute", ["//server/share"]], ["toNamespacedPath", ["C:\\a\\b"]], ["basename", ["C:\\a\\b.txt", ".txt"]]])
  console.log(`win32.${fn}(${JSON.stringify(args).slice(1, -1)}) = ${JSON.stringify(path.win32[fn](...args))}`);
console.log("sep", path.sep, path.delimiter, path.win32.sep, path.win32.delimiter, path === path.posix, path.posix.win32 === path.win32);
for (const bad of [1, null, undefined, {}]) {
  try { path.join("a", bad); } catch (e) { console.log(e.name, e.code, e.message); }
}

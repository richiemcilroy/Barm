// fs loads with its whole API (file access itself needs the natives; see tests/fetch-style program tests)
const fs = require("fs");
const fsp = require("fs/promises");
const names = ["readFileSync", "writeFileSync", "statSync", "existsSync", "readdirSync", "createReadStream", "createWriteStream", "promises", "constants", "watch", "mkdirSync", "realpathSync"];
console.log(names.map((n) => `${n}:${typeof fs[n]}`).join(" "));
console.log(["readFile", "writeFile", "stat", "readdir", "mkdir", "rm", "open"].map((n) => `${n}:${typeof fsp[n]}`).join(" "), fs.promises === fsp);
console.log(fs.constants.O_RDONLY, fs.constants.F_OK, fs.constants.R_OK, typeof fs.constants.O_CREAT, typeof fs.Stats, typeof fs.Dirent, typeof fs.ReadStream);

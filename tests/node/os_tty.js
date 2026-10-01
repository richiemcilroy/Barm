// os and tty: the API's shape, and what doesn't depend on the machine
const os = require("os");
const tty = require("tty");
console.log(os.platform(), os.arch(), os.type(), JSON.stringify(os.EOL), os.endianness(), os.devNull, typeof os.release(), typeof os.hostname(), typeof os.homedir(), typeof os.tmpdir());
console.log(typeof os.totalmem(), typeof os.freemem(), typeof os.uptime(), Array.isArray(os.loadavg()), os.loadavg().length, Array.isArray(os.cpus()), typeof os.networkInterfaces(), typeof os.availableParallelism());
console.log(Object.keys(os.userInfo()).sort().join(","), typeof os.constants.signals.SIGINT, os.constants.signals.SIGTERM, typeof os.constants.errno.ENOENT, typeof os.machine(), typeof os.version(), typeof os.getPriority());
console.log(tty.isatty(-1), tty.isatty(1.5), typeof tty.WriteStream, typeof tty.ReadStream, typeof tty.isatty(1));

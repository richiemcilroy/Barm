// child_process loads: its functions and their promisified forms (nothing is spawned)
const cp = require('child_process');
const { promisify } = require('util');
console.log('functions', ['spawn', 'spawnSync', 'exec', 'execSync', 'execFile', 'execFileSync', 'fork'].map((k) => typeof cp[k]).join(','));
console.log('promisified', typeof promisify(cp.execFile), typeof promisify(cp.exec), typeof cp.ChildProcess);

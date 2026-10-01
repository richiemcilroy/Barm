// cluster loads: this process is the primary
const cluster = require('cluster');
console.log('cluster', cluster.isPrimary, cluster.isWorker, typeof cluster.fork, typeof cluster.setupPrimary);

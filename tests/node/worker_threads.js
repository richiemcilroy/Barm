// worker_threads on the main thread: its identity, MessageChannel, environment data
const wt = require('worker_threads');
console.log('main', wt.isMainThread, wt.threadId, wt.parentPort, wt.workerData, typeof wt.Worker, typeof wt.resourceLimits);
wt.setEnvironmentData('k', 'v');
console.log('env', wt.getEnvironmentData('k'));
const { port1, port2 } = new wt.MessageChannel();
port2.on('message', (m) => {
  console.log('message', JSON.stringify(m));
  port1.close();
  port2.close();
});
port1.postMessage({ hello: [1, 2] });

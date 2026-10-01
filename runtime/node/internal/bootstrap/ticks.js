'use strict';

// The tick queue, set up on first use (by process.nextTick or the timers), once: setupTaskQueue
// installs the callback that runs it.

let queue;
module.exports = function taskQueue() {
  return queue ??= require('internal/process/task_queues').setupTaskQueue();
};

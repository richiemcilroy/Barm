'use strict';

// internalBinding('worker'): this thread's identity (the main thread, which owns the process) and
// resource-limit indices. Starting a Worker isn't supported yet: its constructor throws.

class Worker {
  constructor() {
    const e = new Error('worker_threads.Worker is not supported by Tov yet');
    e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
    throw e;
  }
}

module.exports = {
  Worker,
  getEnvMessagePort: () => undefined,
  threadId: 0,
  threadName: '',
  isMainThread: true,
  isInternalThread: false,
  ownsProcessState: true,
  kMaxYoungGenerationSizeMb: 0,
  kMaxOldGenerationSizeMb: 1,
  kCodeRangeSizeMb: 2,
  kStackSizeMb: 3,
  kTotalResourceLimitCount: 4,
};

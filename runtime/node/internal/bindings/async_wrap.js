'use strict';

// internalBinding('async_wrap'): the shared fields async_hooks reads and writes (as Node.js lays
// them out in src/async_wrap.h), and the slow paths for its async id stack. Tov's own handles
// (timers, sockets) don't report async hook events yet; JS-created resources do.

const constants = {
  // async_hook_fields
  kInit: 0,
  kBefore: 1,
  kAfter: 2,
  kDestroy: 3,
  kPromiseResolve: 4,
  kTotals: 5,
  kCheck: 6,
  kStackLength: 7,
  kUsesExecutionAsyncResource: 8,
  // async_id_fields
  kExecutionAsyncId: 0,
  kTriggerAsyncId: 1,
  kAsyncIdCounter: 2,
  kDefaultTriggerAsyncId: 3,
};

const async_hook_fields = new Uint32Array(9);
const async_id_fields = new Float64Array(4);
// the main context is async id 1, triggered by 0; checks on by default
async_id_fields[constants.kExecutionAsyncId] = 1;
async_id_fields[constants.kAsyncIdCounter] = 1;
async_id_fields[constants.kDefaultTriggerAsyncId] = -1;
async_hook_fields[constants.kCheck] = 1;

// (execution id, trigger id) pairs: the fast path in JS uses this array while it has room
let async_ids_stack = new Float64Array(16 * 2);
const execution_async_resources = [];

const binding = {
  constants,
  async_hook_fields,
  async_id_fields,
  execution_async_resources,
  get async_ids_stack() {
    return async_ids_stack;
  },
  // the stack outgrew the array: grow it, then push as the JS fast path does
  pushAsyncContext(asyncId, triggerAsyncId) {
    const offset = async_hook_fields[constants.kStackLength];
    if (offset * 2 >= async_ids_stack.length) {
      const bigger = new Float64Array(async_ids_stack.length * 2);
      bigger.set(async_ids_stack);
      async_ids_stack = bigger;
    }
    async_ids_stack[offset * 2] = async_id_fields[constants.kExecutionAsyncId];
    async_ids_stack[offset * 2 + 1] = async_id_fields[constants.kTriggerAsyncId];
    async_hook_fields[constants.kStackLength]++;
    async_id_fields[constants.kExecutionAsyncId] = asyncId;
    async_id_fields[constants.kTriggerAsyncId] = triggerAsyncId;
  },
  // a pop that doesn't match the current id: Node.js reports it and aborts
  popAsyncContext(asyncId) {
    const current = async_id_fields[constants.kExecutionAsyncId];
    const msg = `Error: async hook stack has become corrupted (actual: ${current}, expected: ${asyncId})`;
    try {
      process.stderr.write(`${msg}\n`);
    } catch {}
    throw new Error(msg);
  },
  clearAsyncIdStack() {
    async_id_fields[constants.kExecutionAsyncId] = 0;
    async_id_fields[constants.kTriggerAsyncId] = 0;
    async_hook_fields[constants.kStackLength] = 0;
    execution_async_resources.length = 0;
  },
  executionAsyncResource(index) {
    return execution_async_resources[index];
  },
  setupHooks() {},
  setCallbackTrampoline() {},
  queueDestroyAsyncId() {},
  registerDestroyHook() {},
  setPromiseHooks() {},
  getPromiseHooks: () => [undefined, undefined, undefined, undefined],
  Providers: { NONE: 0 },
};

module.exports = binding;

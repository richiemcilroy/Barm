'use strict';

// internalBinding('performance'): the clock behind performance.now() (milliseconds since the
// program started). A native will give a monotonic clock; until then the engine's.

const NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP = 0;
const NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN = 1;
const start = Date.now();
const engineNow = typeof globalThis.performance?.now === 'function' ? globalThis.performance.now.bind(globalThis.performance) : null;
const base = engineNow ? engineNow() : 0;
const now = engineNow ? () => engineNow() - base : () => Date.now() - start;

const milestones = new Float64Array(8).fill(-1);
milestones[NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP] = start * 1e3;
milestones[NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN] = 0;

module.exports = {
  constants: { NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP, NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN },
  milestones,
  now,
  observerCounts: new Uint32Array(16),
};

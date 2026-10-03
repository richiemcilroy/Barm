'use strict';

// internalBinding('performance'): Node.js's src/node_perf.cc and histogram.cc. The clock is the
// program's monotonic one (native hrtime, nanoseconds since it started); milestones are
// nanoseconds on that clock, as Node.js's are on its own. Histograms are exact (a count per
// value) where Node.js's are HdrHistogram's approximations. Barm reports no garbage collection
// entries.

const native = globalThis.__barm_native;
const hrtime = native?.hrtime
  ? () => native.hrtime()
  : (() => {
    const engineNow = typeof globalThis.performance?.now === 'function' ? globalThis.performance.now.bind(globalThis.performance) : () => Date.now();
    const base = engineNow();
    return () => (engineNow() - base) * 1e6;
  })();

const constants = {
  NODE_PERFORMANCE_GC_MAJOR: 4,
  NODE_PERFORMANCE_GC_MINOR: 1,
  NODE_PERFORMANCE_GC_INCREMENTAL: 8,
  NODE_PERFORMANCE_GC_WEAKCB: 16,
  NODE_PERFORMANCE_GC_FLAGS_NO: 0,
  NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED: 2,
  NODE_PERFORMANCE_GC_FLAGS_FORCED: 4,
  NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING: 8,
  NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE: 16,
  NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY: 32,
  NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE: 64,
  NODE_PERFORMANCE_ENTRY_TYPE_GC: 0,
  NODE_PERFORMANCE_ENTRY_TYPE_HTTP: 1,
  NODE_PERFORMANCE_ENTRY_TYPE_HTTP2: 2,
  NODE_PERFORMANCE_ENTRY_TYPE_NET: 3,
  NODE_PERFORMANCE_ENTRY_TYPE_DNS: 4,
  NODE_PERFORMANCE_ENTRY_TYPE_QUIC: 5,
  NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP: 0,
  NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN: 1,
  NODE_PERFORMANCE_MILESTONE_ENVIRONMENT: 2,
  NODE_PERFORMANCE_MILESTONE_NODE_START: 3,
  NODE_PERFORMANCE_MILESTONE_V8_START: 4,
  NODE_PERFORMANCE_MILESTONE_LOOP_START: 5,
  NODE_PERFORMANCE_MILESTONE_LOOP_EXIT: 6,
  NODE_PERFORMANCE_MILESTONE_BOOTSTRAP_COMPLETE: 7,
};

// the program started at 0 on its clock; the time origin is then, in µs since the epoch
const milestones = new Float64Array(8).fill(-1);
milestones[0] = (Date.now() - hrtime() / 1e6) * 1e3;
milestones[1] = 0;
milestones[2] = 0;
milestones[3] = 0;
milestones[4] = 0;
milestones[5] = 0;

// milliseconds since the time origin
const now = native?.nowMs ?? (() => hrtime() / 1e6);

const kAdd = Symbol('kAdd');

// a histogram of positive integers
class Histogram {
  #counts = new Map();
  #sorted = null;
  #count = 0;
  #min = 9223372036854776000;
  #max = 0;
  #sum = 0;
  #sumSquares = 0;
  #exceeds = 0;
  #lowest;
  #highest;
  #prevDelta = 0;

  constructor(lowest = 1, highest = Number.MAX_SAFE_INTEGER) {
    this.#lowest = Number(lowest);
    this.#highest = Number(highest);
  }

  record(value) {
    const v = Number(value);
    if (v < this.#lowest || v > this.#highest) {
      this.#exceeds++;
      return;
    }
    this.#counts.set(v, (this.#counts.get(v) ?? 0) + 1);
    this.#sorted = null;
    this.#count++;
    if (v < this.#min) this.#min = v;
    if (v > this.#max) this.#max = v;
    this.#sum += v;
    this.#sumSquares += v * v;
  }

  recordDelta() {
    const t = hrtime();
    if (this.#prevDelta > 0) this.record(Math.max(1, Math.round(t - this.#prevDelta)));
    this.#prevDelta = t;
  }

  add(other) {
    const h = other?.[kAdd] ? other : null;
    if (h) h[kAdd](this);
  }

  [kAdd](into) {
    for (const [v, n] of this.#counts) for (let i = 0; i < n; i++) into.record(v);
  }

  reset() {
    this.#counts.clear();
    this.#sorted = null;
    this.#count = 0;
    this.#min = 9223372036854776000;
    this.#max = 0;
    this.#sum = 0;
    this.#sumSquares = 0;
    this.#exceeds = 0;
    this.#prevDelta = 0;
  }

  count() { return this.#count; }
  countBigInt() { return BigInt(this.#count); }
  min() { return this.#min; }
  minBigInt() { return BigInt(this.#min); }
  max() { return this.#max; }
  maxBigInt() { return BigInt(this.#max); }
  mean() { return this.#count ? this.#sum / this.#count : NaN; }
  stddev() {
    if (!this.#count) return NaN;
    const m = this.#sum / this.#count;
    return Math.sqrt(Math.max(0, this.#sumSquares / this.#count - m * m));
  }
  exceeds() { return this.#exceeds; }
  exceedsBigInt() { return BigInt(this.#exceeds); }

  #keys() {
    this.#sorted ??= [...this.#counts.keys()].sort((a, b) => a - b);
    return this.#sorted;
  }

  percentile(p) {
    if (!this.#count) return 0;
    const want = Math.max(1, Math.ceil((p / 100) * this.#count));
    let seen = 0;
    for (const v of this.#keys()) {
      seen += this.#counts.get(v);
      if (seen >= want) return v;
    }
    return this.#max;
  }
  percentileBigInt(p) { return BigInt(this.percentile(p)); }

  // as HdrHistogram's percentile iteration reports them: 0, 50, 75, 87.5, ... 100
  percentiles(map) {
    if (!this.#count) {
      map.set(100, 0);
      return;
    }
    map.set(0, this.#min);
    let p = 50;
    let step = 50;
    while (p < 100 && step > 1e-9) {
      const v = this.percentile(p);
      map.set(p, v);
      if (v >= this.#max) break;
      step /= 2;
      p += step;
    }
    map.set(100, this.#max);
  }
  percentilesBigInt(map) {
    const m = new Map();
    this.percentiles(m);
    for (const [k, v] of m) map.set(k, BigInt(v));
  }
}

// monitorEventLoopDelay: samples how late a repeating timer fires
class ELDHistogram extends Histogram {
  #timer = null;
  #resolution;

  constructor(resolution) {
    super();
    this.#resolution = resolution;
  }

  start() {
    if (this.#timer !== null) return false;
    const { setTimeout, clearTimeout } = require('timers');
    const ms = Math.max(1, Number(this.#resolution) || 10);
    let expected = hrtime() + ms * 1e6;
    const tick = () => {
      const t = hrtime();
      this.record(Math.max(1, Math.round(t - expected + ms * 1e6)));
      expected = t + ms * 1e6;
      this.#timer = setTimeout(tick, ms);
      this.#timer.unref?.();
    };
    this.#timer = setTimeout(tick, ms);
    this.#timer.unref?.();
    this.#clear = clearTimeout;
    return true;
  }

  #clear = null;

  stop() {
    if (this.#timer === null) return false;
    this.#clear(this.#timer);
    this.#timer = null;
    return true;
  }
}

let observerCallback = null;

module.exports = {
  Histogram,
  constants,
  milestones,
  now,
  observerCounts: new Uint32Array(6),
  setupObservers(fn) {
    observerCallback = fn;
  },
  installGarbageCollectionTracking() {},
  removeGarbageCollectionTracking() {},
  notify() {},
  loopIdleTime: () => 0,
  createELDHistogram: (resolution) => new ELDHistogram(resolution),
  markBootstrapComplete() {
    milestones[constants.NODE_PERFORMANCE_MILESTONE_BOOTSTRAP_COMPLETE] = hrtime();
  },
  uvMetricsInfo: () => [0, 0, 0],
  // (for the host: an entry for observers, as Node.js's C++ reports them)
  getObserverCallback: () => observerCallback,
};

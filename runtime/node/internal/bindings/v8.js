'use strict';

// internalBinding('v8'): heap statistics and flags (Node.js's src/node_v8.cc). The engine is
// JavaScriptCore: the heap's live size and capacity come from it (native heapStats); V8's
// spaces, code statistics, flags and profilers have no counterpart, so they read as empty.

const native = globalThis.__barm_native;

const kHeapSpaces = ['read_only_space', 'new_space', 'old_space', 'code_space', 'shared_space', 'trusted_space', 'shared_trusted_space',
  'new_large_object_space', 'large_object_space', 'code_large_object_space', 'shared_large_object_space',
  'shared_trusted_large_object_space', 'trusted_large_object_space'];

const heapStatisticsBuffer = new Float64Array(15);
const heapCodeStatisticsBuffer = new Float64Array(4);
const heapSpaceStatisticsBuffer = new Float64Array(4);

// (V8's default limit for a 64-bit process)
const kHeapSizeLimit = 4345298944;

function updateHeapStatisticsBuffer() {
  const [live, capacity, extra] = native?.heapStats ? native.heapStats() : [0, 0, 0];
  const used = live || capacity;
  const b = heapStatisticsBuffer;
  b[0] = capacity; // total_heap_size
  b[1] = 0; // total_heap_size_executable
  b[2] = capacity; // total_physical_size
  b[3] = kHeapSizeLimit - used; // total_available_size
  b[4] = used; // used_heap_size
  b[5] = kHeapSizeLimit; // heap_size_limit
  b[6] = 0; // malloced_memory
  b[7] = 0; // peak_malloced_memory
  b[8] = 0; // does_zap_garbage
  b[9] = 1; // number_of_native_contexts
  b[10] = 0; // number_of_detached_contexts
  b[11] = 0; // total_global_handles_size
  b[12] = 0; // used_global_handles_size
  b[13] = extra; // external_memory
  b[14] = used; // total_allocated_bytes
}

function updateHeapSpaceStatisticsBuffer() {
  heapSpaceStatisticsBuffer.fill(0);
}

function updateHeapCodeStatisticsBuffer() {
  heapCodeStatisticsBuffer.fill(0);
}

class GCProfiler {
  start() {}

  stop() {
    return undefined;
  }
}

module.exports = {
  heapStatisticsBuffer,
  heapCodeStatisticsBuffer,
  heapSpaceStatisticsBuffer,
  cachedDataVersionTag: () => 0x8d7a_57ee,
  setHeapSnapshotNearHeapLimit() {},
  updateHeapStatisticsBuffer,
  updateHeapCodeStatisticsBuffer,
  updateHeapSpaceStatisticsBuffer,
  getCppHeapStatistics: () => ({
    committed_size_bytes: 0,
    resident_size_bytes: 0,
    used_size_bytes: 0,
    space_statistics: [],
    type_names: [],
    detail_level: 'brief',
  }),
  kHeapSpaces,
  kTotalHeapSizeIndex: 0,
  kTotalHeapSizeExecutableIndex: 1,
  kTotalPhysicalSizeIndex: 2,
  kTotalAvailableSize: 3,
  kUsedHeapSizeIndex: 4,
  kHeapSizeLimitIndex: 5,
  kMallocedMemoryIndex: 6,
  kPeakMallocedMemoryIndex: 7,
  kDoesZapGarbageIndex: 8,
  kNumberOfNativeContextsIndex: 9,
  kNumberOfDetachedContextsIndex: 10,
  kTotalGlobalHandlesSizeIndex: 11,
  kUsedGlobalHandlesSizeIndex: 12,
  kExternalMemoryIndex: 13,
  kTotalAllocatedBytes: 14,
  kCodeAndMetadataSizeIndex: 0,
  kBytecodeAndMetadataSizeIndex: 1,
  kExternalScriptSourceSizeIndex: 2,
  kCPUProfilerMetaDataSizeIndex: 3,
  kSpaceSizeIndex: 0,
  kSpaceUsedSizeIndex: 1,
  kSpaceAvailableSizeIndex: 2,
  kPhysicalSpaceSizeIndex: 3,
  // (V8's flags don't apply: ignored)
  setFlagsFromString() {},
  startCpuProfile() {},
  stopCpuProfile() {},
  startHeapProfile() {},
  stopHeapProfile() {},
  kSamplingNoFlags: 0,
  kSamplingForceGC: 1,
  kSamplingIncludeObjectsCollectedByMajorGC: 2,
  kSamplingIncludeObjectsCollectedByMinorGC: 4,
  isStringOneByteRepresentation: (s) => !/[^\x00-\xff]/.test(s),
  getHashSeed: () => 0,
  GCProfiler,
  detailLevel: { DETAILED: 1, BRIEF: 0 },
};

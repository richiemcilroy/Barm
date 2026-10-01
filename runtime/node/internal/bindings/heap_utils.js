'use strict';

// internalBinding('heap_utils'): V8's heap snapshots, which JavaScriptCore can't write in V8's
// format; taking one throws ERR_METHOD_NOT_IMPLEMENTED.

function unsupported(name) {
  return function () {
    const e = new Error(`v8.${name}() is not supported by Barm (JavaScriptCore has no V8 heap snapshots)`);
    e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
    throw e;
  };
}

module.exports = {
  buildEmbedderGraph: () => [],
  triggerHeapSnapshot: unsupported('writeHeapSnapshot'),
  createHeapSnapshotStream: unsupported('getHeapSnapshot'),
};

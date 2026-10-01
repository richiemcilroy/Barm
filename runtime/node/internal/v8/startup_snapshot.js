'use strict';

// internal/v8/startup_snapshot: Barm doesn't build V8 startup snapshots, so nothing is ever being
// built and the callbacks never run.
module.exports = {
  runDeserializeCallbacks() {},
  throwIfBuildingSnapshot() {},
  namespace: {
    addDeserializeCallback() {},
    addSerializeCallback() {},
    setDeserializeMainFunction() {},
    isBuildingSnapshot: () => false,
  },
  addAfterUserSerializeCallback() {},
};

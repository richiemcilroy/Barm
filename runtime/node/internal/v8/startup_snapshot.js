'use strict';

// internal/v8/startup_snapshot: Barm doesn't build V8 startup snapshots.
module.exports = {
  namespace: {
    isBuildingSnapshot: () => false,
    addSerializeCallback() {},
    addDeserializeCallback() {},
    setDeserializeMainFunction() {},
  },
};

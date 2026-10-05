'use strict';

// internalBinding('internal_only_v8'): v8.queryObjects, which needs V8's heap walk; Tov finds
// no objects.

module.exports = {
  queryObjects: () => [],
};

'use strict';

// internalBinding('config'): without ICU, util.inspect measures string widths in JS.
module.exports = { hasIntl: false, hasInspector: false, noBrowserGlobals: false, bits: 64 };

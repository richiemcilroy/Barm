'use strict';

// internal/util/trace_sigint (util.setTraceSigInt, behind Node.js's --trace-sigint): printing a
// stack trace on SIGINT needs V8's watchdog, which Barm doesn't have; enabling it does nothing.

const { validateBoolean } = require('internal/validators');

function setTraceSigInt(enable) {
  validateBoolean(enable, 'enable');
}

module.exports = { setTraceSigInt };

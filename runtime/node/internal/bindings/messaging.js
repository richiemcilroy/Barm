'use strict';

// internalBinding('messaging'): DOMException (Node.js's own, from internal/per_context). The
// MessagePort and structuredClone parts come with worker_threads.
const { DOMException, QuotaExceededError } = require('internal/per_context/domexception');
module.exports = { DOMException, QuotaExceededError };

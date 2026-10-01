// Error.prepareStackTrace and CallSites, as depd and callsites use them
const path = require('path');
function getStack() {
  const limit = Error.stackTraceLimit;
  const obj = {};
  const prep = Error.prepareStackTrace;
  Error.prepareStackTrace = (o, stack) => stack;
  Error.stackTraceLimit = Math.max(10, limit);
  Error.captureStackTrace(obj);
  const stack = obj.stack.slice(1);
  Error.prepareStackTrace = prep;
  Error.stackTraceLimit = limit;
  return stack;
}
function depdLike() {
  const site = getStack()[0];
  return [site.getFunctionName(), path.basename(site.getFileName()), site.getLineNumber(), typeof site.getColumnNumber(), site.isEval(), site.isNative()].join(' ');
}
console.log('depd', depdLike());
function callsites() {
  const prev = Error.prepareStackTrace;
  try {
    let result = [];
    Error.prepareStackTrace = (_, sites) => {
      result = sites.slice(1);
      return result;
    };
    new Error().stack;
    return result;
  } finally {
    Error.prepareStackTrace = prev;
  }
}
function caller() {
  return callsites().slice(0, 1).map((s) => `${s.getFunctionName()} ${path.basename(s.getFileName())}:${s.getLineNumber()}`).join(',');
}
console.log('callsites', caller());
console.log('restored', new Error('x') instanceof Error, typeof new Error('x').stack);
Error.prepareStackTrace = (err, sites) => `${err.message} at ${sites.length > 0}`;
const e = new Error('custom');
console.log('formatted', e.stack, e instanceof Error, Object.prototype.toString.call(e));
class MyError extends Error {}
console.log('subclass', new MyError('m').stack, new MyError('m') instanceof MyError);
Error.prepareStackTrace = undefined;
console.log('native', typeof new Error('y').stack === 'string', Error.name);

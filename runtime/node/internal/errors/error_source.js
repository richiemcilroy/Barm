'use strict';

// internal/errors/error_source (Barm's own; Node.js's uses V8's source positions and acorn):
// the source expression an error points at, for assert's "The expression evaluated to a falsy
// value" message. The call site comes from the error's stack; the line from the program's
// source, which the host provides as globalThis.__barm_source(file) (the bundle in a Barm
// program, the file in tests).

// "fn@file:line:col" (JavaScriptCore: col is the call's "(", 1-based) or "at fn (file:line:col)"
const frameRe = /^(?:.*?@|\s*at (?:.*\()?)(.*):(\d+):(\d+)\)?$/;

function getErrorSourceLocation(error) {
  const stack = typeof error?.stack === 'string' ? error.stack : '';
  const source = globalThis.__barm_source;
  if (typeof source !== 'function') return undefined;
  for (const frame of stack.split('\n')) {
    const m = frameRe.exec(frame);
    if (!m) continue;
    let text;
    try {
      text = source(m[1]);
    } catch {
      return undefined;
    }
    if (typeof text !== 'string') return undefined;
    const sourceLine = text.split('\n')[Number(m[2]) - 1];
    if (sourceLine === undefined) return undefined;
    return { sourceLine, startColumn: Number(m[3]) - 1 };
  }
  return undefined;
}

const identChar = /[\w$]/;

// The call at `col` (its "("), from the start of the callee's member chain (`assert.ok`) to the
// call's closing parenthesis
function getFirstExpression(line, col) {
  if (line[col] !== '(') {
    // a position at the callee's name (as V8 reports it): move to its "("
    const open = line.indexOf('(', col);
    if (open < 0) return undefined;
    col = open;
  }
  let start = col;
  // back over spaces, identifiers, ".", "?." and bracketed member names
  for (let i = col - 1; i >= 0; i--) {
    const c = line[i];
    if (identChar.test(c) || c === '.' || c === '?' || c === ' ') {
      if (c !== ' ' && c !== '?') start = i;
      continue;
    }
    if (c === ']') {
      let depth = 0;
      for (; i >= 0; i--) {
        if (line[i] === ']') depth++;
        else if (line[i] === '[' && --depth === 0) break;
      }
      start = Math.max(i, 0);
      continue;
    }
    break;
  }
  // forward to the matching ")", past strings
  let depth = 0;
  let quote = '';
  for (let i = col; i < line.length; i++) {
    const c = line[i];
    if (quote) {
      if (c === '\\') i++;
      else if (c === quote) quote = '';
      continue;
    }
    if (c === '"' || c === "'" || c === '`') quote = c;
    else if (c === '(') depth++;
    else if (c === ')' && --depth === 0) return line.slice(start, i + 1);
  }
  return undefined;
}

function getErrorSourceExpression(error) {
  const loc = getErrorSourceLocation(error);
  if (loc === undefined) return undefined;
  return getFirstExpression(loc.sourceLine, loc.startColumn);
}

module.exports = {
  getErrorSourceLocation,
  getErrorSourceExpression,
};

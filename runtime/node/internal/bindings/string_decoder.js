'use strict';

// internalBinding('string_decoder'): Node.js's src/string_decoder.cc in JS. A decoder's state
// lives in a 7-byte buffer, laid out as Node.js lays it out: up to 4 bytes of an incomplete
// character, how many bytes it still misses, how many are buffered, and the encoding.

const buffer = require('internal/bindings/buffer');

// the encodings, indexed as Node.js's `enum encoding` is
const encodings = ['ascii', 'utf8', 'base64', 'utf16le', 'latin1', 'hex', 'buffer', 'base64url'];
const ASCII = 0, UTF8 = 1, BASE64 = 2, UCS2 = 3, LATIN1 = 4, HEX = 5, BASE64URL = 7;

const kIncompleteCharactersStart = 0;
const kIncompleteCharactersEnd = 4;
const kMissingBytes = 4;
const kBufferedBytes = 5;
const kEncodingField = 6;
const kNumFields = 7;
const kSize = 7;

const slices = {
  [ASCII]: buffer.asciiSlice,
  [UTF8]: buffer.utf8Slice,
  [BASE64]: buffer.base64Slice,
  [UCS2]: buffer.ucs2Slice,
  [LATIN1]: buffer.latin1Slice,
  [HEX]: buffer.hexSlice,
  [BASE64URL]: buffer.base64urlSlice,
};

function makeString(bytes, start, end, enc) {
  return (slices[enc] ?? buffer.utf8Slice)(bytes, start, end);
}

function decode(state, chunk) {
  const enc = state[kEncodingField];
  const data = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
  let start = 0;
  let nread = data.length;
  if (enc !== UTF8 && enc !== UCS2 && enc !== BASE64 && enc !== BASE64URL) {
    return makeString(data, 0, nread, enc);
  }
  let prepend = null;
  // bytes that finish a character from the previous chunk
  if (state[kMissingBytes] > 0) {
    if (enc === UTF8) {
      // a byte that should continue the character but doesn't ends it here (the rest of the
      // character comes out as U+FFFD), and starts a new one
      for (let i = 0; i < nread && i < state[kMissingBytes]; i++) {
        if ((data[start + i] & 0xc0) !== 0x80) {
          state[kMissingBytes] = 0;
          state.set(data.subarray(start, start + i), kIncompleteCharactersStart + state[kBufferedBytes]);
          state[kBufferedBytes] += i;
          start += i;
          nread -= i;
          break;
        }
      }
    }
    const found = Math.min(nread, state[kMissingBytes]);
    state.set(data.subarray(start, start + found), kIncompleteCharactersStart + state[kBufferedBytes]);
    start += found;
    nread -= found;
    state[kMissingBytes] -= found;
    state[kBufferedBytes] += found;
    if (state[kMissingBytes] === 0) {
      prepend = makeString(state, kIncompleteCharactersStart, kIncompleteCharactersStart + state[kBufferedBytes], enc);
      state[kBufferedBytes] = 0;
    }
  }
  if (nread === 0) return prepend ?? '';
  // a character cut off at the end of this chunk waits for the next
  if (enc === UTF8 && data[start + nread - 1] & 0x80) {
    for (let i = nread - 1; ; i--) {
      state[kBufferedBytes]++;
      const b = data[start + i];
      if ((b & 0xc0) === 0x80) {
        // a trailing byte: more than 3, or the chunk began with it, means no character to finish
        if (state[kBufferedBytes] >= 4 || i === 0) {
          state[kBufferedBytes] = 0;
          break;
        }
      } else {
        if ((b & 0xe0) === 0xc0) state[kMissingBytes] = 2;
        else if ((b & 0xf0) === 0xe0) state[kMissingBytes] = 3;
        else if ((b & 0xf8) === 0xf0) state[kMissingBytes] = 4;
        else {
          state[kBufferedBytes] = 0;
          break;
        }
        if (state[kBufferedBytes] >= state[kMissingBytes]) {
          // complete (or invalid anyway): nothing to hold back
          state[kMissingBytes] = 0;
          state[kBufferedBytes] = 0;
        }
        state[kMissingBytes] -= state[kBufferedBytes];
        break;
      }
    }
  } else if (enc === UCS2) {
    if (nread % 2 === 1) {
      state[kBufferedBytes] = 1;
      state[kMissingBytes] = 1;
    } else if ((data[start + nread - 1] & 0xfc) === 0xd8) {
      // the first half of a surrogate pair
      state[kBufferedBytes] = 2;
      state[kMissingBytes] = 2;
    }
  } else if (enc === BASE64 || enc === BASE64URL) {
    state[kBufferedBytes] = nread % 3;
    if (state[kBufferedBytes] > 0) state[kMissingBytes] = 3 - state[kBufferedBytes];
  }
  if (state[kBufferedBytes] > 0) {
    nread -= state[kBufferedBytes];
    state.set(data.subarray(start + nread, start + nread + state[kBufferedBytes]), kIncompleteCharactersStart);
  }
  const body = nread > 0 ? makeString(data, start, start + nread, enc) : '';
  return prepend === null ? body : prepend + body;
}

function flush(state) {
  const enc = state[kEncodingField];
  if (enc === UCS2 && state[kBufferedBytes] % 2 === 1) {
    // a lone trailing byte is dropped
    state[kMissingBytes]--;
    state[kBufferedBytes]--;
  }
  if (state[kBufferedBytes] === 0) return '';
  const s = makeString(state, kIncompleteCharactersStart, kIncompleteCharactersStart + state[kBufferedBytes], enc);
  state[kMissingBytes] = 0;
  state[kBufferedBytes] = 0;
  return s;
}

module.exports = {
  encodings,
  kIncompleteCharactersStart,
  kIncompleteCharactersEnd,
  kMissingBytes,
  kBufferedBytes,
  kEncodingField,
  kNumFields,
  kSize,
  decode,
  flush,
};

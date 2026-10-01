'use strict';

// internalBinding('http_parser'): the method lists and HTTPParser's constants (Node.js's
// src/node_http_parser.cc, over llhttp). Parsing needs sockets, which Barm's net doesn't have
// yet: an HTTPParser can be made, and using one throws.

const methods = ['DELETE', 'GET', 'HEAD', 'POST', 'PUT', 'CONNECT', 'OPTIONS', 'TRACE', 'COPY', 'LOCK', 'MKCOL', 'MOVE',
  'PROPFIND', 'PROPPATCH', 'SEARCH', 'UNLOCK', 'BIND', 'REBIND', 'UNBIND', 'ACL', 'REPORT', 'MKACTIVITY', 'CHECKOUT', 'MERGE',
  'M-SEARCH', 'NOTIFY', 'SUBSCRIBE', 'UNSUBSCRIBE', 'PATCH', 'PURGE', 'MKCALENDAR', 'LINK', 'UNLINK', 'SOURCE', 'QUERY'];
const allMethods = [...methods.slice(0, -1), 'PRI', 'DESCRIBE', 'ANNOUNCE', 'SETUP', 'PLAY', 'PAUSE', 'TEARDOWN', 'GET_PARAMETER',
  'SET_PARAMETER', 'REDIRECT', 'RECORD', 'FLUSH', 'QUERY'];

function notYet() {
  const e = new Error('HTTP parsing is not supported by Barm yet');
  e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
  throw e;
}

class HTTPParser {
  static REQUEST = 1;
  static RESPONSE = 2;
  static kOnMessageBegin = 0;
  static kOnHeaders = 1;
  static kOnHeadersComplete = 2;
  static kOnBody = 3;
  static kOnMessageComplete = 4;
  static kOnExecute = 5;
  static kOnTimeout = 6;
  static kLenientNone = 0;
  static kLenientHeaders = 1;
  static kLenientChunkedLength = 2;
  static kLenientKeepAlive = 4;
  static kLenientTransferEncoding = 8;
  static kLenientVersion = 16;
  static kLenientDataAfterClose = 32;
  static kLenientOptionalLFAfterCR = 64;
  static kLenientOptionalCRLFAfterChunk = 128;
  static kLenientOptionalCRBeforeLF = 256;
  static kLenientSpacesAfterChunkSize = 512;
  static kLenientHeaderValueRelaxed = 1024;
  static kLenientAll = 1023;

  close() {}
  free() {}
  remove() {}
  execute() { notYet(); }
  finish() { notYet(); }
  initialize() {}
  pause() {}
  resume() {}
  consume() {}
  unconsume() {}
  getCurrentBuffer() { notYet(); }
}

class ConnectionsList {
  all() { return []; }
  idle() { return []; }
  active() { return []; }
  expired() { return []; }
}

module.exports = { HTTPParser, ConnectionsList, methods, allMethods };

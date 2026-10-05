'use strict';

// internalBinding('http_parser'): HTTP/1.x parsing behind Node.js's http (src/node_http_parser.cc,
// over llhttp), in JavaScript. execute(bytes) parses as they arrive, across any split: the start
// line and headers (raw [name, value, ...]), then the body by Content-Length, chunked (with
// trailers) or, for responses, until the connection ends; messages follow one another on a
// connection. It calls the parser's kOn* callbacks as Node.js's does and fails with llhttp's
// error codes.

const methods = ['DELETE', 'GET', 'HEAD', 'POST', 'PUT', 'CONNECT', 'OPTIONS', 'TRACE', 'COPY', 'LOCK', 'MKCOL', 'MOVE',
  'PROPFIND', 'PROPPATCH', 'SEARCH', 'UNLOCK', 'BIND', 'REBIND', 'UNBIND', 'ACL', 'REPORT', 'MKACTIVITY', 'CHECKOUT', 'MERGE',
  'M-SEARCH', 'NOTIFY', 'SUBSCRIBE', 'UNSUBSCRIBE', 'PATCH', 'PURGE', 'MKCALENDAR', 'LINK', 'UNLINK', 'SOURCE', 'QUERY'];
const allMethods = [...methods.slice(0, -1), 'PRI', 'DESCRIBE', 'ANNOUNCE', 'SETUP', 'PLAY', 'PAUSE', 'TEARDOWN', 'GET_PARAMETER',
  'SET_PARAMETER', 'REDIRECT', 'RECORD', 'FLUSH', 'QUERY'];
const methodIndex = new Map(allMethods.map((m, i) => [m, i]));

const REQUEST = 1;
const RESPONSE = 2;
const kOnMessageBegin = 0;
const kOnHeaders = 1;
const kOnHeadersComplete = 2;
const kOnBody = 3;
const kOnMessageComplete = 4;
const kOnExecute = 5;
const kOnTimeout = 6;
const kLenientHeaders = 1;

const DEFAULT_MAX_HEADER_SIZE = 16384;

// parser states
const START = 0;
const HEADERS = 1;
const BODY_LENGTH = 2;
const CHUNK_SIZE = 3;
const CHUNK_DATA = 4;
const CHUNK_END = 5;
const TRAILERS = 6;
const BODY_EOF = 7;
const DONE = 8; // an upgrade: nothing more is parsed

let Buffer;
let FastBuffer;

// the header field characters (RFC 9110 tchar)
const TOKEN = new Uint8Array(256);
for (const c of "!#$%&'*+-.^_`|~") TOKEN[c.charCodeAt(0)] = 1;
for (let c = 48; c < 58; c++) TOKEN[c] = 1;
for (let c = 65; c < 91; c++) TOKEN[c] = 1;
for (let c = 97; c < 123; c++) TOKEN[c] = 1;

function latin1(b, start, end) {
  if (end - start < 64) {
    let s = '';
    for (let i = start; i < end; i++) s += String.fromCharCode(b[i]);
    return s;
  }
  return b.latin1Slice(start, end);
}

class ParseError extends Error {}

function parseError(code, reason) {
  const e = new ParseError(reason);
  e.code = code;
  e.reason = reason;
  return e;
}

class HTTPParser {
  static REQUEST = REQUEST;
  static RESPONSE = RESPONSE;
  static kOnMessageBegin = kOnMessageBegin;
  static kOnHeaders = kOnHeaders;
  static kOnHeadersComplete = kOnHeadersComplete;
  static kOnBody = kOnBody;
  static kOnMessageComplete = kOnMessageComplete;
  static kOnExecute = kOnExecute;
  static kOnTimeout = kOnTimeout;
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

  constructor() {
    this.#reset(REQUEST);
  }

  #type = REQUEST;
  #maxHeaderSize = DEFAULT_MAX_HEADER_SIZE;
  #lenient = 0;
  #state = START;
  #line = null; // bytes of an unfinished line (start line, header, chunk size, trailer)
  #headerBytes = 0;
  #headers = [];
  #versionMajor = 1;
  #versionMinor = 1;
  #method = '';
  #url = '';
  #status = 0;
  #statusMessage = '';
  #remaining = 0; // body or chunk bytes left
  #keepAlive = true;
  #upgrade = false;
  #current = null;
  #paused = false;
  #list = null;
  // (for ConnectionsList: in a message since when, and whether its headers are in)
  _tovStart = 0;
  _tovHeadersDone = false;

  #reset(type) {
    this.#type = type;
    this.#state = START;
    this.#line = null;
  }

  initialize(type, resource, maxHeaderSize, lenient, connections) {
    this.#reset(type);
    this._tovStart = 0;
    if (connections instanceof ConnectionsList) {
      this.#list = connections;
      connections.push(this);
    }
    this.#maxHeaderSize = maxHeaderSize > 0 ? maxHeaderSize : DEFAULT_MAX_HEADER_SIZE;
    this.#lenient = lenient | 0;
    this.#paused = false;
  }

  close() {}

  free() {}

  remove() {
    this.#list?.pop(this);
    this.#list = null;
  }

  pause() {
    this.#paused = true;
  }

  resume() {
    this.#paused = false;
  }

  // (reads come to execute() from the socket's 'data' events: there's no stream to consume)
  consume() {}

  unconsume() {}

  getCurrentBuffer() {
    Buffer ??= require('buffer').Buffer;
    return this.#current ?? Buffer.alloc(0);
  }

  // finish(): the connection ended; completes a body read until then
  finish() {
    if (this.#state === BODY_EOF) {
      this.#state = START;
      this[kOnMessageComplete]?.();
      return undefined;
    }
    if (this.#state === START && this.#line === null) return undefined;
    if (this.#state === DONE) return undefined;
    const e = parseError('HPE_INVALID_EOF_STATE', 'Invalid EOF state');
    e.bytesParsed = 0;
    return e;
  }

  execute(data) {
    this.#current = data;
    try {
      const n = this.#execute(data);
      return n;
    } catch (e) {
      if (!(e instanceof ParseError)) throw e;
      const err = new Error(`Parse Error: ${e.reason}`);
      err.code = e.code;
      err.reason = e.reason;
      err.bytesParsed = e.bytesParsed ?? 0;
      return err;
    } finally {
      this.#current = null;
    }
  }

  // the end of the line at i (its \n), or -1
  #lineEnd(b, i) {
    const n = b.indexOf(10, i);
    return n;
  }

  // the line in b[start, end) (without its CR), joined with what came before it
  #takeLine(b, start, end) {
    let stop = end;
    if (stop > start && b[stop - 1] === 13) stop--;
    if (this.#line === null) return [b, start, stop];
    FastBuffer ??= require('internal/buffer').FastBuffer;
    const prev = this.#line;
    this.#line = null;
    const joined = new FastBuffer(prev.length + (stop - start));
    joined.set(prev, 0);
    joined.set(b.subarray(start, stop), prev.length);
    let len = joined.length;
    if (len > 0 && joined[len - 1] === 13 && stop === end) len--;
    return [joined, 0, len];
  }

  #keep(b, start) {
    FastBuffer ??= require('internal/buffer').FastBuffer;
    const rest = b.subarray(start);
    if (this.#line === null) {
      this.#line = new FastBuffer(rest.length);
      this.#line.set(rest);
    } else {
      const joined = new FastBuffer(this.#line.length + rest.length);
      joined.set(this.#line, 0);
      joined.set(rest, this.#line.length);
      this.#line = joined;
    }
  }

  #countHeader(n, at) {
    this.#headerBytes += n;
    if (this.#headerBytes > this.#maxHeaderSize) {
      const e = parseError('HPE_HEADER_OVERFLOW', 'Header overflow');
      e.bytesParsed = at;
      throw e;
    }
  }

  #execute(b) {
    let i = 0;
    const len = b.length;
    while (i < len) {
      switch (this.#state) {
        case START:
        case HEADERS:
        case TRAILERS: {
          const nl = this.#lineEnd(b, i);
          if (nl < 0) {
            this.#countHeader(len - i, i);
            this.#keep(b, i);
            return len;
          }
          this.#countHeader(nl + 1 - i, i);
          const [lb, ls, le] = this.#takeLine(b, i, nl);
          i = nl + 1;
          if (this.#state === START) {
            // (empty lines before a message are skipped)
            if (le === ls) continue;
            this.#startLine(lb, ls, le, i);
          } else if (le === ls) {
            if (this.#state === TRAILERS) {
              if (this.#headers.length) this[kOnHeaders]?.(this.#headers, '');
              this.#complete();
            } else {
              const stop = this.#headersComplete(i);
              if (stop) return i;
            }
          } else {
            this.#header(lb, ls, le, i);
          }
          break;
        }
        case BODY_LENGTH: {
          const n = Math.min(this.#remaining, len - i);
          this[kOnBody]?.(b.subarray(i, i + n));
          this.#remaining -= n;
          i += n;
          if (this.#remaining === 0) this.#complete();
          break;
        }
        case BODY_EOF: {
          this[kOnBody]?.(b.subarray(i));
          return len;
        }
        case CHUNK_SIZE: {
          const nl = this.#lineEnd(b, i);
          if (nl < 0) {
            this.#keep(b, i);
            if (this.#line.length > 1024) this.#fail('HPE_INVALID_CHUNK_SIZE', 'Invalid character in chunk size', i);
            return len;
          }
          const [lb, ls, le] = this.#takeLine(b, i, nl);
          i = nl + 1;
          let size = 0;
          let digits = 0;
          let p = ls;
          for (; p < le; p++) {
            const c = lb[p];
            const v = c >= 48 && c <= 57 ? c - 48 : c >= 97 && c <= 102 ? c - 87 : c >= 65 && c <= 70 ? c - 55 : -1;
            if (v < 0) break;
            size = size * 16 + v;
            digits++;
          }
          // (chunk extensions after ';' are ignored)
          while (p < le && (lb[p] === 32 || lb[p] === 9)) p++;
          if (digits === 0 || (p < le && lb[p] !== 59) || size > Number.MAX_SAFE_INTEGER) {
            this.#fail('HPE_INVALID_CHUNK_SIZE', 'Invalid character in chunk size', i);
          }
          if (size === 0) {
            this.#state = TRAILERS;
            this.#headers = [];
            this.#headerBytes = 0;
          } else {
            this.#remaining = size;
            this.#state = CHUNK_DATA;
          }
          break;
        }
        case CHUNK_DATA: {
          const n = Math.min(this.#remaining, len - i);
          this[kOnBody]?.(b.subarray(i, i + n));
          this.#remaining -= n;
          i += n;
          if (this.#remaining === 0) this.#state = CHUNK_END;
          break;
        }
        case CHUNK_END: {
          // CRLF after the data
          const c = b[i];
          if (c === 13) {
            i++;
          } else if (c === 10) {
            i++;
            this.#state = CHUNK_SIZE;
          } else {
            this.#fail('HPE_STRICT', 'Expected LF after chunk data', i);
          }
          break;
        }
        case DONE:
          return i;
      }
    }
    return len;
  }

  #fail(code, reason, at) {
    const e = parseError(code, reason);
    e.bytesParsed = at;
    throw e;
  }

  #version(s, at) {
    const m = /^HTTP\/(\d)\.(\d)$/.exec(s);
    if (!m) this.#fail('HPE_INVALID_VERSION', 'Invalid HTTP version', at);
    this.#versionMajor = Number(m[1]);
    this.#versionMinor = Number(m[2]);
  }

  #startLine(b, s, e, at) {
    this.#headers = [];
    this.#upgrade = false;
    this._tovStart = Date.now();
    this._tovHeadersDone = false;
    this[kOnMessageBegin]?.();
    const line = latin1(b, s, e);
    if (this.#type === REQUEST) {
      const sp1 = line.indexOf(' ');
      const sp2 = line.lastIndexOf(' ');
      if (sp1 <= 0) this.#fail('HPE_INVALID_METHOD', 'Invalid method encountered', at);
      const method = line.slice(0, sp1);
      if (!methodIndex.has(method)) this.#fail('HPE_INVALID_METHOD', 'Invalid method encountered', at);
      if (sp2 <= sp1) this.#fail('HPE_INVALID_URL', 'Invalid URL', at);
      this.#method = method;
      this.#url = line.slice(sp1 + 1, sp2);
      if (this.#url === '' || /[\x00-\x20\x7f]/.test(this.#url)) this.#fail('HPE_INVALID_URL', 'Invalid URL', at);
      this.#version(line.slice(sp2 + 1), at);
    } else {
      if (!line.startsWith('HTTP/')) this.#fail('HPE_INVALID_CONSTANT', 'Expected HTTP/', at);
      const sp1 = line.indexOf(' ');
      this.#version(sp1 < 0 ? line : line.slice(0, sp1), at);
      const rest = sp1 < 0 ? '' : line.slice(sp1 + 1);
      const status = rest.slice(0, 3);
      if (!/^\d{3}$/.test(status)) this.#fail('HPE_INVALID_STATUS', 'Invalid status code', at);
      this.#status = Number(status);
      this.#statusMessage = rest.length > 4 ? rest.slice(4) : '';
    }
    this.#state = HEADERS;
  }

  #header(b, s, e, at) {
    // (an obsolete folded continuation line joins the previous value)
    if ((b[s] === 32 || b[s] === 9) && this.#headers.length) {
      const h = this.#headers;
      h[h.length - 1] = `${h[h.length - 1]} ${latin1(b, s, e).trim()}`;
      return;
    }
    let colon = -1;
    for (let p = s; p < e; p++) {
      const c = b[p];
      if (c === 58) {
        colon = p;
        break;
      }
      if (!TOKEN[c]) this.#fail('HPE_INVALID_HEADER_TOKEN', 'Invalid header field char', at);
    }
    if (colon <= s) this.#fail('HPE_INVALID_HEADER_TOKEN', 'Invalid header field char', at);
    let vs = colon + 1;
    let ve = e;
    while (vs < ve && (b[vs] === 32 || b[vs] === 9)) vs++;
    while (ve > vs && (b[ve - 1] === 32 || b[ve - 1] === 9)) ve--;
    if (!(this.#lenient & kLenientHeaders)) {
      for (let p = vs; p < ve; p++) {
        const c = b[p];
        if ((c < 32 && c !== 9) || c === 127) this.#fail('HPE_INVALID_HEADER_TOKEN', 'Invalid header value char', at);
      }
    }
    this.#headers.push(latin1(b, s, colon), latin1(b, vs, ve));
  }

  // the headers are in: what follows them; true if parsing stops here (an upgrade)
  #headersComplete(at) {
    const h = this.#headers;
    let contentLength = -1;
    let chunked = false;
    let connection = '';
    let hasUpgrade = false;
    let hasTE = false;
    for (let k = 0; k < h.length; k += 2) {
      const name = h[k].toLowerCase();
      const value = h[k + 1];
      if (name === 'content-length') {
        if (!/^\d+$/.test(value)) this.#fail('HPE_INVALID_CONTENT_LENGTH', 'Invalid character in Content-Length', at);
        const n = Number(value);
        if (contentLength >= 0 && contentLength !== n) this.#fail('HPE_UNEXPECTED_CONTENT_LENGTH', 'Duplicate Content-Length', at);
        contentLength = n;
      } else if (name === 'transfer-encoding') {
        hasTE = true;
        const codings = value.toLowerCase().split(',');
        chunked = codings[codings.length - 1].trim() === 'chunked';
      } else if (name === 'connection') {
        connection += `${value.toLowerCase()},`;
      } else if (name === 'upgrade') {
        hasUpgrade = true;
      }
    }
    if (hasTE && contentLength >= 0 && !(this.#lenient & 2)) {
      this.#fail('HPE_UNEXPECTED_CONTENT_LENGTH', 'Content-Length can\'t be present with Transfer-Encoding', at);
    }
    const tokens = connection.split(',').map((t) => t.trim());
    const http11 = this.#versionMajor > 1 || (this.#versionMajor === 1 && this.#versionMinor >= 1);
    let keepAlive = http11 ? !tokens.includes('close') : tokens.includes('keep-alive');
    let upgrade;
    if (this.#type === REQUEST) {
      upgrade = this.#method === 'CONNECT' || (hasUpgrade && tokens.includes('upgrade'));
    } else {
      upgrade = this.#status === 101;
    }
    // a response read until the connection closes can't keep it open
    let noBody = false;
    if (this.#type === RESPONSE) {
      const s = this.#status;
      noBody = (s >= 100 && s < 200) || s === 204 || s === 304;
      if (!noBody && !chunked && contentLength < 0) keepAlive = false;
    } else if (!chunked && contentLength < 0) {
      noBody = true;
    }
    if (hasTE && !chunked && this.#type === REQUEST) {
      this.#fail('HPE_INVALID_TRANSFER_ENCODING', 'Request has invalid `Transfer-Encoding`', at);
    }
    this.#keepAlive = keepAlive;
    this.#upgrade = upgrade;
    const headers = this.#headers;
    this.#headers = [];
    const method = this.#type === REQUEST ? methodIndex.get(this.#method) : undefined;
    const ret = this[kOnHeadersComplete]?.(this.#versionMajor, this.#versionMinor, headers, method,
      this.#type === REQUEST ? this.#url : undefined, this.#type === RESPONSE ? this.#status : undefined,
      this.#type === RESPONSE ? this.#statusMessage : undefined, upgrade, keepAlive);
    this.#headerBytes = 0;
    this._tovHeadersDone = true;
    if (ret === 2 || (upgrade && ret !== 1 && (this.#type === RESPONSE || this.#method === 'CONNECT' || noBody))) {
      // the rest of the connection isn't HTTP
      this.#upgrade = true;
      this.#state = DONE;
      this[kOnMessageComplete]?.();
      return true;
    }
    if (ret === 1 || noBody || contentLength === 0) {
      this.#complete();
      return false;
    }
    if (chunked) {
      this.#state = CHUNK_SIZE;
    } else if (contentLength > 0) {
      this.#remaining = contentLength;
      this.#state = BODY_LENGTH;
    } else {
      this.#state = BODY_EOF;
    }
    return false;
  }

  #complete() {
    this._tovStart = 0;
    this.#state = this.#upgrade ? DONE : START;
    this.#headerBytes = 0;
    this[kOnMessageComplete]?.();
  }
}

// a server's connections (their parsers), for closing idle ones and timing out slow requests
class ConnectionsList {
  #all = new Set();

  push(parser) {
    this.#all.add(parser);
  }

  pop(parser) {
    this.#all.delete(parser);
  }

  all() {
    return [...this.#all];
  }

  // between messages
  idle() {
    return [...this.#all].filter((p) => p._tovStart === 0);
  }

  active() {
    return [...this.#all].filter((p) => p._tovStart !== 0);
  }

  // in a message too long: its headers past headersTimeout, or all of it past requestTimeout
  expired(headersTimeout, requestTimeout) {
    const now = Date.now();
    const out = [];
    for (const p of this.#all) {
      if (p._tovStart === 0) continue;
      const age = now - p._tovStart;
      if ((!p._tovHeadersDone && headersTimeout > 0 && age > headersTimeout) || (requestTimeout > 0 && age > requestTimeout)) {
        out.push(p);
        this.#all.delete(p);
      }
    }
    return out;
  }
}

module.exports = { HTTPParser, ConnectionsList, methods, allMethods };

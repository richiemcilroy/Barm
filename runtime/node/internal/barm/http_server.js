'use strict';

// http.Server on Barm's native HTTP server (runtime/barm.c, through runtime/node.c), as Bun runs
// node:http on its own: the native server accepts connections, parses requests and keeps
// connections alive; each request comes to JavaScript as Node.js's own IncomingMessage and
// ServerResponse, whose formatted bytes go back out raw. A server listens this way when it can:
// on a TCP port, with no 'connection' listeners (they need a socket per connection); otherwise
// it listens as Node.js's does, on net.
//
// Each request's socket is a stand-in (NativeSocket): what the response writes is gathered and
// sent when the tick ends (with the end, if it ends by then, in one call). An upgrade (or
// CONNECT) with listeners takes the connection over as a real net.Socket.

const EventEmitter = require('events');
const { Buffer } = require('buffer');

const native = globalThis.__barm_native?.http;
const streams = globalThis.__barm_native?.stream;

let net;
let isIP;

const kId = Symbol('kId');
const kChunks = Symbol('kChunks');
const kFlushQueued = Symbol('kFlushQueued');
const kDone = Symbol('kDone');
const kAddress = Symbol('kAddress');

// the request's socket: writes go to the native server for request id
class NativeSocket extends EventEmitter {
  constructor(server, id) {
    super();
    this[kId] = id;
    this[kChunks] = [];
    this[kFlushQueued] = false;
    this[kDone] = false;
    this[kAddress] = null;
    this.server = server;
    this._httpMessage = null;
    this.writable = true;
    this.readable = false;
    this.destroyed = false;
    this.encrypted = undefined;
    this.connecting = false;
    this.bytesWritten = 0;
    this.writableLength = 0;
    this.writableHighWaterMark = 16384;
    this.writableNeedDrain = false;
    this.writableCorked = 0;
    this._writableState = { corked: 0, length: 0, needDrain: false, ended: false };
    this._paused = false;
  }

  // (chunks are strings with their encodings, or bytes: strings go as they are, encoded natively)
  #flush(end) {
    if (this[kDone]) return;
    const chunks = this[kChunks];
    this[kChunks] = [];
    if (end) this[kDone] = true;
    if (chunks.length === 0) {
      native.write(this[kId], null, end, false);
    } else if (chunks.length === 2 && typeof chunks[0] === 'string') {
      native.write(this[kId], chunks[0], end, chunks[1] === 'latin1');
    } else {
      const parts = [];
      for (let i = 0; i < chunks.length; i += 2) {
        parts.push(typeof chunks[i] === 'string' ? Buffer.from(chunks[i], chunks[i + 1]) : chunks[i]);
      }
      native.write(this[kId], parts.length === 1 ? parts[0] : Buffer.concat(parts), end, false);
    }
  }

  write(data, encoding, callback) {
    if (typeof encoding === 'function') {
      callback = encoding;
      encoding = undefined;
    }
    if (this[kDone]) return false;
    if (typeof data === 'string') {
      const enc = encoding === 'latin1' || encoding === 'binary' || encoding === 'ascii' ? 'latin1' : (encoding ?? 'utf8');
      this[kChunks].push(data, enc);
      // (characters: exact for ASCII, which is what responses mostly are; encoding happens natively)
      this.bytesWritten += data.length;
    } else {
      this[kChunks].push(data, null);
      this.bytesWritten += data.byteLength;
    }
    if (typeof callback === 'function') process.nextTick(callback);
    // (more of a response that hasn't ended goes out when the tick ends)
    if (!this[kFlushQueued]) {
      this[kFlushQueued] = true;
      process.nextTick(() => {
        this[kFlushQueued] = false;
        if (this[kChunks].length) this.#flush(0);
      });
    }
    return true;
  }

  // the response is complete: what's gathered goes with the end (2: then close the connection)
  _barmEnd(close) {
    this.#flush(close ? 2 : 1);
  }

  end(data, encoding, callback) {
    if (typeof data === 'function') {
      callback = data;
      data = undefined;
    }
    if (data !== undefined && data !== null) this.write(data, encoding);
    this._barmEnd(true);
    if (typeof callback === 'function') process.nextTick(callback);
    return this;
  }

  destroy(err) {
    if (!this.destroyed) {
      this.destroyed = true;
      this.writable = false;
      this._barmEnd(true);
      process.nextTick(() => {
        if (err) this.emit('error', err);
        this.emit('close', !!err);
      });
    }
    return this;
  }

  destroySoon() {
    this._barmEnd(true);
  }

  cork() {}
  uncork() {}
  setTimeout(ms, callback) {
    if (typeof callback === 'function') this.once('timeout', callback);
    return this;
  }
  setNoDelay() { return this; }
  setKeepAlive() { return this; }
  ref() { return this; }
  unref() { return this; }
  pause() { return this; }
  resume() { return this; }
  _onTimeout() {}

  #addresses() {
    if (this[kAddress]) return this[kAddress];
    const fd = native.fd(this[kId]);
    const peer = fd >= 0 ? streams.name(fd, true) : null;
    const local = fd >= 0 ? streams.name(fd, false) : null;
    this[kAddress] = {
      remote: Array.isArray(peer) ? { address: peer[0], family: peer[1], port: peer[2] } : {},
      local: Array.isArray(local) ? { address: local[0], family: local[1], port: local[2] } : {},
    };
    return this[kAddress];
  }

  get remoteAddress() { return this.#addresses().remote.address; }
  get remoteFamily() { return this.#addresses().remote.family; }
  get remotePort() { return this.#addresses().remote.port; }
  get localAddress() { return this.#addresses().local.address; }
  get localFamily() { return this.#addresses().local.family; }
  get localPort() { return this.#addresses().local.port; }

  address() {
    const l = this.#addresses().local;
    return { address: l.address, family: l.family, port: l.port };
  }

  get readyState() {
    return this.destroyed ? 'closed' : 'open';
  }
}

// the request's headers, as [name, value, ...]
function rawHeaders(wire) {
  const out = [];
  let i = 0;
  const n = wire.length;
  while (i < n) {
    let e = wire.indexOf('\n', i);
    if (e < 0) e = n;
    const end = e > i && wire.charCodeAt(e - 1) === 13 ? e - 1 : e;
    const c = wire.indexOf(':', i);
    if (c > i && c < end) {
      let vs = c + 1;
      while (vs < end && (wire.charCodeAt(vs) === 32 || wire.charCodeAt(vs) === 9)) vs++;
      let ve = end;
      while (ve > vs && (wire.charCodeAt(ve - 1) === 32 || wire.charCodeAt(ve - 1) === 9)) ve--;
      out.push(wire.slice(i, c), wire.slice(vs, ve));
    }
    i = e + 1;
  }
  return out;
}

function install(Server, { IncomingMessage, kServerResponse, kIncomingMessage, kUniqueHeaders, continueExpression }) {
  if (!native) return;
  net ??= require('net');
  const netListen = net.Server.prototype.listen;

  const { allMethods } = require('internal/bindings/http_parser');
  const methods = new Set(allMethods);
  const BAD_REQUEST = Buffer.from('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');

  function onRequest(server, id, method, target, wire, body, http10) {
    // (a method llhttp doesn't know: Node.js answers 400 and closes)
    if (!methods.has(method)) {
      native.write(id, BAD_REQUEST, 2, false);
      return;
    }
    const raw = rawHeaders(wire);
    let connection = '';
    let upgrade = false;
    let expect;
    for (let k = 0; k < raw.length; k += 2) {
      const name = raw[k].toLowerCase();
      if (name === 'connection') connection += `${raw[k + 1].toLowerCase()},`;
      else if (name === 'upgrade') upgrade = true;
      else if (name === 'expect') expect = raw[k + 1];
    }
    const keepAlive = http10 ? connection.includes('keep-alive') : !/(^|,)\s*close\s*(,|$)/.test(connection);
    const isUpgrade = method === 'CONNECT' || (upgrade && /(^|,)\s*upgrade\s*(,|$)/.test(connection));
    const event = method === 'CONNECT' ? 'connect' : 'upgrade';
    if (isUpgrade && server.listenerCount(event) > 0) {
      const taken = native.takeover(id);
      if (taken) {
        const { TCP, constants } = require('internal/bindings/tcp_wrap');
        const handle = new TCP(constants.SOCKET);
        handle.open(taken[0]);
        const socket = new net.Socket({ handle, readable: true, writable: true });
        socket.server = server;
        const req = new (server[kIncomingMessage] ?? IncomingMessage)(socket);
        fill(req, method, target, raw, http10);
        req.upgrade = true;
        if (body) req.push(Buffer.from(body.buffer, body.byteOffset, body.byteLength));
        req.push(null);
        req.complete = true;
        server.emit(event, req, socket, Buffer.from(taken[1].buffer, taken[1].byteOffset, taken[1].byteLength));
        return;
      }
    }
    const socket = new NativeSocket(server, id);
    const req = new (server[kIncomingMessage] ?? IncomingMessage)(socket);
    fill(req, method, target, raw, http10);
    if (body) req.push(Buffer.from(body.buffer, body.byteOffset, body.byteLength));
    req.push(null);
    req.complete = true;
    const res = new server[kServerResponse](req, {
      highWaterMark: socket.writableHighWaterMark,
      rejectNonStandardBodyWrites: server.rejectNonStandardBodyWrites,
    });
    res._keepAliveTimeout = server.keepAliveTimeout;
    res._maxRequestsPerSocket = server.maxRequestsPerSocket;
    res.shouldKeepAlive = keepAlive;
    res[kUniqueHeaders] = server[kUniqueHeaders];
    res.assignSocket(socket);
    res.on('finish', () => {
      if (!req._consuming && !req._readableState.resumeScheduled) req._dump();
      res.detachSocket(socket);
      socket._barmEnd(res._last || !res.shouldKeepAlive);
      process.nextTick(() => {
        if (!res._closed) {
          res.destroyed = true;
          res._closed = true;
          res.emit('close');
        }
      });
    });
    if (http10 === false && server.requireHostHeader && req.headers.host === undefined) {
      res.writeHead(400, ['Connection', 'close']);
      res.end();
      return;
    }
    if (expect !== undefined) {
      if (continueExpression.test(expect)) {
        res._expect_continue = true;
        if (server.listenerCount('checkContinue') > 0) {
          server.emit('checkContinue', req, res);
          return;
        }
      } else if (server.listenerCount('checkExpectation') > 0) {
        server.emit('checkExpectation', req, res);
        return;
      } else {
        res.writeHead(417);
        res.end();
        return;
      }
    }
    server.emit('request', req, res);
  }

  function fill(req, method, target, raw, http10) {
    req.method = method;
    req.url = target;
    req.httpVersionMajor = 1;
    req.httpVersionMinor = http10 ? 0 : 1;
    req.httpVersion = http10 ? '1.0' : '1.1';
    req._addHeaderLines(raw, raw.length);
  }

  // listen(...) on the native server when it can, else as net.Server does
  Server.prototype.listen = function listen(...args) {
    const normalized = net._normalizeArgs(args);
    const options = normalized[0];
    const cb = normalized[1];
    isIP ??= require('internal/net').isIP;
    const port = options.port;
    const host = options.host;
    const eligible = options.path === undefined && options.handle === undefined && options.fd === undefined &&
      !options.exclusive && !options.ipv6Only && options.signal === undefined && options.reusePort !== true &&
      this.listenerCount('connection') <= 1 && this._handle == null &&
      (port === undefined || port === null || (typeof port === 'number' && port >= 0 && port < 65536) || (typeof port === 'string' && /^\d+$/.test(port))) &&
      (host === undefined || host === null || isIP(host) !== 0 || host === 'localhost');
    // (Server's own 'connection' listener is the one http adds)
    if (!eligible) return netListen.apply(this, args);
    const bindHost = host === undefined || host === null ? '::' : host;
    const { callFromHost } = require('internal/bindings/task_queue');
    const id = native.listen(Number(port ?? 0), bindHost, true, (rid, method, target, wire, body, http10) => {
      callFromHost(onRequest, this, rid, method, target, wire, body, http10);
    });
    if (typeof cb === 'function') this.once('listening', cb);
    if (typeof id !== 'number') {
      const e = new Error(`listen EADDRINUSE: address already in use ${bindHost}:${port}`);
      e.code = id.includes('in use') ? 'EADDRINUSE' : 'EACCES';
      e.errno = e.code === 'EADDRINUSE' ? -48 : -13;
      e.syscall = 'listen';
      e.address = bindHost;
      e.port = Number(port ?? 0);
      if (e.code !== 'EADDRINUSE') e.message = `listen ${e.code}: ${id}`;
      process.nextTick(() => this.emit('error', e));
      return this;
    }
    const family = bindHost.includes(':') ? 'IPv6' : 'IPv4';
    const address = bindHost === 'localhost' ? '127.0.0.1' : bindHost;
    this._handle = {
      getsockname(out) {
        out.address = address;
        out.family = family;
        out.port = native.port(id);
        return 0;
      },
      close(callback) {
        native.stop(id, false);
        if (typeof callback === 'function') process.nextTick(callback);
      },
      ref() {},
      unref() {},
      fd: -1,
    };
    this._connectionKey = `${family === 'IPv6' ? 6 : 4}:${address}:${native.port(id)}`;
    process.nextTick(() => this.emit('listening'));
    return this;
  };
}

module.exports = { install, NativeSocket };

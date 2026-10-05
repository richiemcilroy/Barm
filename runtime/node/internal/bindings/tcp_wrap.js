'use strict';

// internalBinding('tcp_wrap'): TCP sockets (Node.js's src/tcp_wrap.cc) as libuv streams over
// Tov's native socket handles (runtime/node.c): bind, listen (onconnection(err, client)),
// connect (req.oncomplete(status, handle, req, readable, writable)), names and options.

const { SocketWrap } = require('internal/bindings/stream_wrap');

const native = globalThis.__tov_native?.stream;

const constants = { SOCKET: 0, SERVER: 1, UV_TCP_IPV6ONLY: 1, UV_TCP_REUSEPORT: 2 };

class TCP extends SocketWrap {
  constructor(type) {
    super();
    this.type = type;
  }

  open(fd) {
    return this._tovOpen(fd);
  }

  bind(address, port) {
    return this._tovBind(address, port, 4, false);
  }

  bind6(address, port, flags) {
    return this._tovBind(address, port, 6, !!(flags & constants.UV_TCP_IPV6ONLY));
  }

  connect(req, address, port) {
    return this._tovConnect(req, address, port, 4);
  }

  connect6(req, address, port) {
    return this._tovConnect(req, address, port, 6);
  }

  setNoDelay(enable) {
    return this.fd >= 0 ? native.option(this.fd, 'nodelay', !!enable, 0) : 0;
  }

  setKeepAlive(enable, delay) {
    return this.fd >= 0 ? native.option(this.fd, 'keepalive', !!enable, delay | 0) : 0;
  }

  setTypeOfService() {
    return 0;
  }

  getTypeOfService() {
    return 0;
  }

  // closes with a reset (RST) instead of a FIN
  reset(callback) {
    if (this.fd >= 0) native.option(this.fd, 'reset', true, 0);
    this.close(callback);
    return 0;
  }
}

class TCPConnectWrap {}

module.exports = { TCP, TCPConnectWrap, constants };

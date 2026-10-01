'use strict';

// internalBinding('pipe_wrap'): pipes (Node.js's src/pipe_wrap.cc) as libuv streams: a
// socketpair to a child process, an fd opened with open(), or a unix domain socket (bind,
// listen, connect by path).

const { SocketWrap } = require('internal/bindings/stream_wrap');

class Pipe extends SocketWrap {
  constructor(type) {
    super();
    this.type = type;
  }

  open(fd) {
    return this._barmOpen(fd);
  }

  bind(path) {
    return this._barmBind(path, 0, 0, false);
  }

  connect(req, path) {
    return this._barmConnect(req, path, 0, 0);
  }

  fchmod() {
    return 0;
  }

  setPendingInstances() {}
}

class PipeConnectWrap {}

module.exports = {
  Pipe,
  PipeConnectWrap,
  constants: { SOCKET: 0, SERVER: 1, IPC: 2, UV_READABLE: 1, UV_WRITABLE: 2 },
};

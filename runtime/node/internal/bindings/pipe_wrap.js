'use strict';

// internalBinding('pipe_wrap'): a pipe (a socketpair to a child process, or an fd opened with
// open()) as a libuv stream. Unix domain sockets (bind, listen, connect) aren't written yet.

const { LibuvStreamWrap } = require('internal/bindings/stream_wrap');

class Pipe extends LibuvStreamWrap {
  constructor(type) {
    super();
    this.type = type;
  }

  open(fd) {
    return this._barmOpen(fd);
  }

  bind() {
    return -45; // UV_ENOTSUP
  }

  listen() {
    return -45;
  }

  connect() {
    return -45;
  }

  fchmod() {
    return 0;
  }
}

class PipeConnectWrap {}

module.exports = {
  Pipe,
  PipeConnectWrap,
  constants: { SOCKET: 0, SERVER: 1, IPC: 2, UV_READABLE: 1, UV_WRITABLE: 2 },
};

'use strict';

// internalBinding('credentials'): environment reads that Node.js keeps safe for setuid programs,
// and the temporary directory (libuv's uv_os_tmpdir: TMPDIR, TMP, TEMP, TEMPDIR, else /tmp).

const env = () => globalThis.process?.env ?? {};

function getTempDir() {
  const e = env();
  let dir = e.TMPDIR || e.TMP || e.TEMP || e.TEMPDIR || '/tmp';
  if (dir.length > 1 && dir.endsWith('/')) dir = dir.slice(0, -1);
  return dir;
}

module.exports = {
  safeGetenv: (name) => env()[name],
  getTempDir,
  implementsPosixCredentials: true,
};

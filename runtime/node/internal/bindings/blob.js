'use strict';

// internalBinding('blob'): Node.js's src/node_blob.cc in JS. A blob's handle is a list of parts,
// each bytes or a range of a file, read in order: its reader hands out a part's bytes (a file's
// in 64 KiB chunks, read as they're pulled) and then the end. pull(callback) calls
// callback(status, ArrayBuffer) at once: 1 with bytes, 0 at the end, -errno when a file can't be
// read (or changed since the blob was made).

const CHUNK = 65536;
const UV_EINVAL = -22;

let fs;

class Reader {
  #parts;
  #i = 0;
  #fd = -1;
  #pos = 0;

  constructor(parts) {
    this.#parts = parts;
  }

  pull(callback) {
    for (;;) {
      const part = this.#parts[this.#i];
      if (part === undefined) return callback(0, undefined);
      if (part.bytes !== undefined) {
        this.#i++;
        const b = part.bytes;
        if (b.byteLength === 0) continue;
        return callback(1, b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));
      }
      if (this.#fd < 0) {
        fs ??= require('fs');
        try {
          this.#fd = fs.openSync(part.path, 'r');
          const st = fs.fstatSync(this.#fd);
          // (a file blob reads the file as it was: one changed since fails, as in Node.js)
          if (st.mtimeMs !== part.mtimeMs || st.size !== part.fileSize) {
            this.#close();
            return callback(UV_EINVAL, undefined);
          }
        } catch (e) {
          this.#close();
          return callback(typeof e.errno === 'number' ? e.errno : UV_EINVAL, undefined);
        }
        this.#pos = part.start;
      }
      const want = Math.min(CHUNK, part.end - this.#pos);
      if (want <= 0) {
        this.#close();
        this.#i++;
        continue;
      }
      const buf = new Uint8Array(want);
      let n;
      try {
        n = fs.readSync(this.#fd, buf, 0, want, this.#pos);
      } catch (e) {
        this.#close();
        return callback(typeof e.errno === 'number' ? e.errno : UV_EINVAL, undefined);
      }
      if (n === 0) {
        // (shorter than it was)
        this.#close();
        return callback(UV_EINVAL, undefined);
      }
      this.#pos += n;
      return callback(1, n === want ? buf.buffer : buf.buffer.slice(0, n));
    }
  }

  // (reads never block, so there's never a wakeup to give)
  setWakeup() {}

  #close() {
    if (this.#fd >= 0) {
      try { fs.closeSync(this.#fd); } catch {}
      this.#fd = -1;
    }
  }
}

class BlobHandle {
  constructor(parts, length) {
    this.parts = parts;
    this.length = length;
  }

  // bytes [start, end) of the blob, as a handle sharing its parts
  slice(start, end) {
    const parts = [];
    let at = 0;
    for (const p of this.parts) {
      const size = p.bytes !== undefined ? p.bytes.byteLength : p.end - p.start;
      const from = Math.max(start, at), to = Math.min(end, at + size);
      if (from < to) {
        if (p.bytes !== undefined) parts.push({ bytes: p.bytes.subarray(from - at, to - at) });
        else parts.push({ ...p, start: p.start + (from - at), end: p.start + (to - at) });
      }
      at += size;
      if (at >= end) break;
    }
    return new BlobHandle(parts, Math.max(0, end - start));
  }

  getReader() {
    return new Reader(this.parts);
  }
}

// createBlob(sources [Uint8Array | handle], length): the bytes are the blob's own (blob.js copies)
function createBlob(sources, length) {
  const parts = [];
  for (const s of sources) {
    if (s instanceof BlobHandle) parts.push(...s.parts);
    else parts.push({ bytes: s instanceof Uint8Array ? s : new Uint8Array(s) });
  }
  return new BlobHandle(parts, length);
}

// createBlobFromFilePath(path) -> [handle, length], or undefined if it isn't a readable file
function createBlobFromFilePath(path) {
  fs ??= require('fs');
  let st;
  try {
    st = fs.statSync(`${path}`);
  } catch {
    return undefined;
  }
  if (!st.isFile()) return undefined;
  const part = { path: `${path}`, start: 0, end: st.size, fileSize: st.size, mtimeMs: st.mtimeMs };
  return [new BlobHandle([part], st.size), st.size];
}

// the buffers (ArrayBuffers or views) as one ArrayBuffer
function concat(buffers) {
  let total = 0;
  for (const b of buffers) total += b.byteLength;
  const out = new Uint8Array(total);
  let at = 0;
  for (const b of buffers) {
    out.set(ArrayBuffer.isView(b) ? new Uint8Array(b.buffer, b.byteOffset, b.byteLength) : new Uint8Array(b), at);
    at += b.byteLength;
  }
  return out.buffer;
}

// blob: URLs (URL.createObjectURL): id -> [handle, length, type]
const objects = new Map();

module.exports = {
  createBlob,
  createBlobFromFilePath,
  concat,
  storeDataObject(id, handle, length, type) {
    objects.set(`${id}`, [handle, length, `${type}`]);
  },
  getDataObject(id) {
    return objects.get(`${id}`);
  },
  revokeObjectURL(url) {
    const prefix = 'blob:nodedata:';
    if (url.startsWith(prefix)) objects.delete(url.slice(prefix.length));
  },
};

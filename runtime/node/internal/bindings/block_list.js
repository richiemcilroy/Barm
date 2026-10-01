'use strict';

// internalBinding('block_list'): SocketAddress and BlockList (Node.js's src/node_sockaddr.cc).
// Addresses compare as 16 bytes, an IPv4 address as its IPv4-mapped IPv6 form, so a rule for
// one matches the other as Node.js's do.

const AF_INET = 2;
const AF_INET6 = process.platform === 'darwin' ? 30 : 10;

let dns;
// an address's 16 bytes, or null
function toBytes(address, family) {
  if (family === AF_INET) {
    const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(address);
    if (!m || m.slice(1).some((o) => Number(o) > 255)) return null;
    return Uint8Array.of(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, ...m.slice(1).map(Number));
  }
  dns ??= globalThis.__barm_native?.dns;
  const b = dns?.ipv6Bytes(address);
  return b ? new Uint8Array(b) : null;
}

function compare(a, b) {
  for (let i = 0; i < 16; i++) if (a[i] !== b[i]) return a[i] - b[i];
  return 0;
}

class SocketAddress {
  #address;
  #port;
  #family;
  #flowlabel;

  constructor(address, port, family, flowlabel) {
    const bytes = toBytes(address, family);
    if (!bytes) {
      const e = new TypeError('Invalid socket address');
      e.code = 'ERR_INVALID_ADDRESS';
      throw e;
    }
    this.bytes = bytes;
    this.#address = family === AF_INET6 ? dns.canonicalizeIP(address) ?? address : address;
    this.#port = port;
    this.#family = family;
    this.#flowlabel = flowlabel;
  }

  detail(out) {
    out.address = this.#address;
    out.port = this.#port;
    out.family = this.#family;
    out.flowlabel = this.#flowlabel;
    return out;
  }

  legacyDetail() {
    return { address: this.#address, port: this.#port, family: this.#family === AF_INET ? 'IPv4' : 'IPv6' };
  }

  flowlabel() {
    return this.#flowlabel;
  }

  get familyName() {
    return this.#family === AF_INET ? 'IPv4' : 'IPv6';
  }

  get text() {
    return this.#address;
  }
}

class BlockList {
  #rules = [];

  addAddress(a) {
    this.#rules.unshift({ text: `Address: ${a.familyName} ${a.text}`, test: (b) => compare(b, a.bytes) === 0 });
  }

  addRange(start, end) {
    if (compare(start.bytes, end.bytes) > 0) return false;
    this.#rules.unshift({
      text: `Range: ${start.familyName} ${start.text}-${end.text}`,
      test: (b) => compare(b, start.bytes) >= 0 && compare(b, end.bytes) <= 0,
    });
    return true;
  }

  addSubnet(network, prefix) {
    // (an IPv4 prefix counts from the mapped form's 96 bits)
    const bits = network.familyName === 'IPv4' ? prefix + 96 : prefix;
    const base = network.bytes;
    this.#rules.unshift({
      text: `Subnet: ${network.familyName} ${network.text}/${prefix}`,
      test: (b) => {
        for (let i = 0; i < 16; i++) {
          const left = bits - i * 8;
          if (left <= 0) return true;
          const mask = left >= 8 ? 0xff : (0xff << (8 - left)) & 0xff;
          if ((b[i] & mask) !== (base[i] & mask)) return false;
        }
        return true;
      },
    });
  }

  check(a) {
    return this.#rules.some((r) => r.test(a.bytes));
  }

  getRules() {
    return this.#rules.map((r) => r.text);
  }
}

module.exports = { BlockList, SocketAddress, AF_INET, AF_INET6 };

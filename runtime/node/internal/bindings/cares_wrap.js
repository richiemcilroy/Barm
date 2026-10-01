'use strict';

// internalBinding('cares_wrap'): Node.js's src/cares_wrap.cc. Lookups (getaddrinfo,
// getnameinfo) run the system resolver on Barm's work queue (globalThis.__barm_native.dns, in
// runtime/node.c). Queries (ChannelWrap's resolve* family, which Node.js sends with c-ares) are
// DNS messages built and read here and sent by the natives over UDP, then TCP when the answer
// is truncated, to the system's name servers (/etc/resolv.conf) or those set with setServers().
// Answers come back in c-ares's shapes, and failures with its error codes.

const native = globalThis.__barm_native?.dns;

function dns() {
  if (native) return native;
  const e = new Error('DNS is not available outside Barm\'s runtime');
  e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
  throw e;
}

// a callback from the loop: then process.nextTick's queue, as Node.js's MakeCallback does
const { callFromHost } = require('internal/bindings/task_queue');
const complete = (fn) => callFromHost(fn);

let domainToASCII;
function ascii(name) {
  if (!/[^\x00-\x7F]/.test(name)) return name;
  domainToASCII ??= require('internal/bindings/url').domainToASCII;
  return domainToASCII(name) || name;
}

class GetAddrInfoReqWrap {}
class GetNameInfoReqWrap {}
class QueryReqWrap {}

// getaddrinfo(req, hostname, family, hints, order): req.oncomplete(err, addresses) later
function getaddrinfo(req, hostname, family, hints, order) {
  return dns().getaddrinfo(ascii(hostname), family, hints, order, (err, addresses) => {
    complete(() => req.oncomplete(err, addresses));
  });
}

// getnameinfo(req, ip, port): req.oncomplete(err, hostname, service) later
function getnameinfo(req, ip, port) {
  return dns().getnameinfo(ip, port, (err, hostname, service) => {
    complete(() => req.oncomplete(err, hostname, service));
  });
}

function canonicalizeIP(ip) {
  return dns().canonicalizeIP(`${ip}`);
}

let Buffer;
function convertIpv6StringToBuffer(ip) {
  const bytes = dns().ipv6Bytes(`${ip}`);
  if (bytes === undefined) return null;
  Buffer ??= require('buffer').Buffer;
  return Buffer.from(bytes.buffer, bytes.byteOffset, 16);
}

// c-ares's ares_strerror, by its status number
const aresMessages = [
  'Successful completion', 'DNS server returned answer with no data', 'DNS server claims query was misformatted',
  'DNS server returned general failure', 'Domain name not found', 'DNS server does not implement requested operation',
  'DNS server refused query', 'Misformatted DNS query', 'Misformatted domain name', 'Unsupported address family',
  'Misformatted DNS reply', 'Could not contact DNS servers', 'Timeout while contacting DNS servers', 'End of file',
  'Error reading file', 'Out of memory', 'Channel is being destroyed', 'Misformatted string',
  'Illegal flags specified', 'Given hostname is not numeric', 'Illegal hints flags specified',
  'c-ares library initialization not yet performed', 'Error loading iphlpapi.dll',
  'Could not find GetNetworkParams function', 'DNS query cancelled',
];
function strerror(code) {
  return aresMessages[code] ?? 'unknown';
}

// ---------------------------------------------------------------- DNS messages

const T = { A: 1, NS: 2, CNAME: 5, SOA: 6, PTR: 12, MX: 15, TXT: 16, AAAA: 28, SRV: 33, NAPTR: 35, TLSA: 52, ANY: 255, CAA: 257 };
const typeNames = { __proto__: null };
for (const k in T) typeNames[T[k]] = k;

function badName() {
  return 'EBADNAME';
}

// the query for name and type, or an error code
function encodeQuery(name, type) {
  name = ascii(name);
  if (name.endsWith('.')) name = name.slice(0, -1);
  const labels = name === '' ? [] : name.split('.');
  let size = 12 + 1 + 4 + 11;
  for (const l of labels) {
    if (l.length === 0 || l.length > 63) return badName();
    size += 1 + l.length;
  }
  if (size - 16 > 255) return badName();
  const b = new Uint8Array(size);
  const id = (Math.random() * 65536) | 0;
  b[0] = id >> 8;
  b[1] = id & 255;
  b[2] = 0x01; // recursion desired
  b[5] = 1; // one question
  b[11] = 1; // one additional record: EDNS
  let o = 12;
  for (const l of labels) {
    b[o++] = l.length;
    for (let i = 0; i < l.length; i++) {
      const c = l.charCodeAt(i);
      if (c > 127) return badName();
      b[o++] = c;
    }
  }
  b[o++] = 0;
  b[o++] = type >> 8;
  b[o++] = type & 255;
  b[o++] = 0;
  b[o++] = 1; // IN
  // OPT: root name, type 41, UDP payload 1232
  b[o++] = 0;
  b[o++] = 0;
  b[o++] = 41;
  b[o++] = 1232 >> 8;
  b[o++] = 1232 & 255;
  return b;
}

function latin1(b, start, end) {
  let s = '';
  for (let i = start; i < end; i++) s += String.fromCharCode(b[i]);
  return s;
}

class Reader {
  constructor(b) {
    this.b = b;
    this.v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  }

  u8(o) {
    if (o >= this.b.length) throw new RangeError('short');
    return this.b[o];
  }

  u16(o) {
    return this.v.getUint16(o);
  }

  u32(o) {
    return this.v.getUint32(o);
  }

  // a (possibly compressed) name at o: [name, offset after it]
  name(o) {
    const labels = [];
    let end = -1;
    for (let jumps = 0; jumps < 128;) {
      const len = this.u8(o);
      if (len === 0) {
        if (end < 0) end = o + 1;
        return [labels.join('.'), end];
      }
      if ((len & 0xc0) === 0xc0) {
        if (end < 0) end = o + 2;
        o = ((len & 0x3f) << 8) | this.u8(o + 1);
        jumps++;
        continue;
      }
      let label = '';
      for (let i = 1; i <= len; i++) {
        const c = this.u8(o + i);
        label += c === 0x2e || c === 0x5c ? `\\${String.fromCharCode(c)}` : String.fromCharCode(c);
      }
      labels.push(label);
      o += len + 1;
    }
    throw new RangeError('loop');
  }

  // a character-string at o: [string, offset after it]
  text(o) {
    const len = this.u8(o);
    return [latin1(this.b, o + 1, o + 1 + len), o + 1 + len];
  }
}

function ipv4(b, o) {
  return `${b[o]}.${b[o + 1]}.${b[o + 2]}.${b[o + 3]}`;
}

function ipv6(b, o) {
  const groups = [];
  for (let i = 0; i < 16; i += 2) groups.push(((b[o + i] << 8) | b[o + i + 1]).toString(16));
  return dns().canonicalizeIP(groups.join(':'));
}

// one record's data as c-ares's parsers give it
function readRecord(r, type, o, len) {
  const b = r.b;
  switch (type) {
    case T.A: return { address: ipv4(b, o) };
    case T.AAAA: return { address: ipv6(b, o) };
    case T.CNAME: case T.NS: case T.PTR: return { value: r.name(o)[0] };
    case T.MX: return { priority: r.u16(o), exchange: r.name(o + 2)[0] };
    case T.TXT: {
      const entries = [];
      for (let p = o; p < o + len;) {
        const [s, next] = r.text(p);
        entries.push(s);
        p = next;
      }
      return { entries };
    }
    case T.SRV: return { priority: r.u16(o), weight: r.u16(o + 2), port: r.u16(o + 4), name: r.name(o + 6)[0] };
    case T.SOA: {
      const [nsname, p1] = r.name(o);
      const [hostmaster, p2] = r.name(p1);
      return { nsname, hostmaster, serial: r.u32(p2), refresh: r.u32(p2 + 4), retry: r.u32(p2 + 8), expire: r.u32(p2 + 12), minttl: r.u32(p2 + 16) };
    }
    case T.NAPTR: {
      const order = r.u16(o);
      const preference = r.u16(o + 2);
      const [flags, p1] = r.text(o + 4);
      const [service, p2] = r.text(p1);
      const [regexp, p3] = r.text(p2);
      const replacement = r.name(p3)[0];
      return { flags, service, regexp, replacement, order, preference };
    }
    case T.CAA: {
      const critical = r.u8(o);
      const tagLen = r.u8(o + 1);
      const tag = latin1(b, o + 2, o + 2 + tagLen);
      const value = latin1(b, o + 2 + tagLen, o + len);
      return { critical, [tag]: value };
    }
    case T.TLSA:
      return { certUsage: r.u8(o), selector: r.u8(o + 1), match: r.u8(o + 2), data: b.slice(o + 3, o + len).buffer };
    default: return null;
  }
}

const rcodes = [null, 'EFORMERR', 'ESERVFAIL', 'ENOTFOUND', 'ENOTIMP', 'EREFUSED'];

// the answer's records ([{ type, ttl, ...data }]), or an error code
function decodeAnswer(answer) {
  try {
    const r = new Reader(answer);
    const rcode = r.u8(3) & 0x0f;
    if (rcode !== 0) return rcodes[rcode] ?? 'EBADRESP';
    const qd = r.u16(4);
    const an = r.u16(6);
    let o = 12;
    for (let i = 0; i < qd; i++) o = r.name(o)[1] + 4;
    const records = [];
    for (let i = 0; i < an; i++) {
      o = r.name(o)[1];
      const type = r.u16(o);
      const ttl = r.u32(o + 4);
      const len = r.u16(o + 8);
      o += 10;
      if (o + len > answer.length) return 'EBADRESP';
      const data = readRecord(r, type, o, len);
      if (data) records.push({ type, ttl, ...data });
      o += len;
    }
    return records;
  } catch {
    return 'EBADRESP';
  }
}

// a query's result as the binding's oncomplete(err, result, ttls) takes it
const shapes = {
  A: (rs) => [rs.map((x) => x.address), rs.map((x) => x.ttl)],
  AAAA: (rs) => [rs.map((x) => x.address), rs.map((x) => x.ttl)],
  CNAME: (rs) => [rs.map((x) => x.value)],
  NS: (rs) => [rs.map((x) => x.value)],
  PTR: (rs) => [rs.map((x) => x.value)],
  MX: (rs) => [rs.map((x) => ({ exchange: x.exchange, priority: x.priority, type: 'MX' }))],
  TXT: (rs) => [rs.map((x) => x.entries)],
  SRV: (rs) => [rs.map((x) => ({ name: x.name, port: x.port, priority: x.priority, weight: x.weight, type: 'SRV' }))],
  NAPTR: (rs) => [rs.map(({ type, ttl, ...x }) => x)],
  SOA: (rs) => [(({ type, ttl, ...x }) => ({ ...x, type: 'SOA' }))(rs[0])],
  // (Node.js 26 puts `type` before the property tag)
  CAA: (rs) => [rs.map(({ type, ttl, critical, ...tag }) => ({ critical, type: 'CAA', ...tag }))],
  TLSA: (rs) => [rs.map(({ type, ttl, ...x }) => x)],
  ANY: (rs) => [rs.map((x) => {
    const { type, ttl, ...data } = x;
    const name = typeNames[type];
    if (type === T.A || type === T.AAAA) return { address: data.address, ttl, type: name };
    if (type === T.CNAME || type === T.NS || type === T.PTR) return { value: data.value, type: name };
    if (type === T.TXT) return { entries: data.entries, type: name };
    return { ...data, type: name };
  })],
};

// the system's name servers
function systemServers() {
  try {
    const text = globalThis.__barm_native.fs.readFileUtf8('/etc/resolv.conf', 0);
    const out = [];
    for (const line of text.split('\n')) {
      const m = /^\s*nameserver\s+(\S+)/.exec(line);
      if (m) out.push([m[1].replace(/%.*$/, ''), 53]);
    }
    if (out.length) return out;
  } catch {
    // (no resolv.conf: the local resolver)
  }
  return [['127.0.0.1', 53]];
}

class ChannelWrap {
  #servers = null;
  #timeout;
  #tries;
  #pending = new Set();

  constructor(timeout, tries) {
    this.#timeout = timeout > 0 ? timeout : 2000;
    this.#tries = tries > 0 ? tries : 4;
  }

  #query(req, name, typeName) {
    const packet = encodeQuery(`${name}`, T[typeName]);
    if (typeof packet === 'string') {
      queueMicrotask(() => complete(() => req.oncomplete(packet)));
      return 0;
    }
    this.#pending.add(req);
    dns().query(this.#servers ??= systemServers(), packet, this.#timeout, this.#tries, (err, answer) => {
      if (!this.#pending.delete(req)) return;
      complete(() => {
        if (err) return req.oncomplete(err);
        const records = decodeAnswer(answer);
        if (typeof records === 'string') return req.oncomplete(records);
        const want = typeName === 'ANY' ? records : records.filter((x) => x.type === T[typeName]);
        if (want.length === 0) return req.oncomplete('ENODATA');
        req.oncomplete(0, ...shapes[typeName](want));
      });
    });
    return 0;
  }

  queryAny(req, name) { return this.#query(req, name, 'ANY'); }
  queryA(req, name) { return this.#query(req, name, 'A'); }
  queryAaaa(req, name) { return this.#query(req, name, 'AAAA'); }
  queryCaa(req, name) { return this.#query(req, name, 'CAA'); }
  queryCname(req, name) { return this.#query(req, name, 'CNAME'); }
  queryMx(req, name) { return this.#query(req, name, 'MX'); }
  queryNs(req, name) { return this.#query(req, name, 'NS'); }
  queryTlsa(req, name) { return this.#query(req, name, 'TLSA'); }
  queryTxt(req, name) { return this.#query(req, name, 'TXT'); }
  querySrv(req, name) { return this.#query(req, name, 'SRV'); }
  queryPtr(req, name) { return this.#query(req, name, 'PTR'); }
  queryNaptr(req, name) { return this.#query(req, name, 'NAPTR'); }
  querySoa(req, name) { return this.#query(req, name, 'SOA'); }

  // reverse: a PTR query for the address's in-addr.arpa or ip6.arpa name
  getHostByAddr(req, ip) {
    let name;
    const v4 = /^(\d+)\.(\d+)\.(\d+)\.(\d+)$/.exec(ip);
    if (v4 && dns().canonicalizeIP(ip) === ip) {
      name = `${v4[4]}.${v4[3]}.${v4[2]}.${v4[1]}.in-addr.arpa`;
    } else {
      const bytes = dns().ipv6Bytes(`${ip}`);
      if (bytes === undefined) return -22; // UV_EINVAL
      const nibbles = [];
      for (let i = 15; i >= 0; i--) nibbles.push((bytes[i] & 15).toString(16), (bytes[i] >> 4).toString(16));
      name = `${nibbles.join('.')}.ip6.arpa`;
    }
    return this.#query(req, name, 'PTR');
  }

  getServers() {
    return (this.#servers ??= systemServers()).map(([ip, port]) => [ip, port]);
  }

  // setServers([[family, ip, port], ...]) -> 0
  setServers(servers) {
    this.#servers = servers.map((s) => [s[1], s[2] || 53]);
    return 0;
  }

  setLocalAddress() {}

  // pending queries fail with ECANCELLED
  cancel() {
    const pending = [...this.#pending];
    this.#pending.clear();
    for (const req of pending) complete(() => req.oncomplete('ECANCELLED'));
  }
}

module.exports = {
  getaddrinfo,
  getnameinfo,
  canonicalizeIP,
  convertIpv6StringToBuffer,
  strerror,
  AF_INET: 2,
  AF_INET6: process.platform === 'darwin' ? 30 : 10,
  AF_UNSPEC: 0,
  AI_ADDRCONFIG: process.platform === 'darwin' ? 1024 : 32,
  AI_ALL: process.platform === 'darwin' ? 256 : 16,
  AI_V4MAPPED: process.platform === 'darwin' ? 2048 : 8,
  DNS_ORDER_VERBATIM: 0,
  DNS_ORDER_IPV4_FIRST: 1,
  DNS_ORDER_IPV6_FIRST: 2,
  GetAddrInfoReqWrap,
  GetNameInfoReqWrap,
  QueryReqWrap,
  ChannelWrap,
};

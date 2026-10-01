'use strict';

// internalBinding('url'): Node.js's src/node_url.cc, which parses with Ada. Here the parser is
// the WHATWG URL Standard's state machine as whatwg-url implements it
// (internal/deps/whatwg-url), and urlComponents are the offsets into the href that Ada's
// url_aggregator reports, which internal/url's getters slice:
//   protocol_end, username_end, host_start, host_end, port, pathname_start, search_start,
//   hash_start, scheme_type (omitted values are 2^32 - 1)

const usm = require('internal/deps/whatwg-url/url-state-machine');

const omitted = 4294967295;
const urlComponents = new Uint32Array(9);
const schemeTypes = { __proto__: null, http: 0, https: 2, ws: 3, ftp: 4, wss: 5, file: 6 };

let ERR_INVALID_URL;
function invalid(input, base) {
  ERR_INVALID_URL ??= require('internal/errors').codes.ERR_INVALID_URL;
  return new ERR_INVALID_URL(input, base);
}

// fills urlComponents for `url` and returns its href
function serialize(url) {
  const href = usm.serializeURL(url);
  const c = urlComponents;
  const protocolEnd = url.scheme.length + 1;
  c[0] = protocolEnd;
  if (url.host !== null) {
    let i = protocolEnd + 2;
    const usernameEnd = i + url.username.length;
    c[1] = usernameEnd;
    let hostStart = usernameEnd;
    if (url.password !== '') hostStart = usernameEnd + 1 + url.password.length;
    c[2] = hostStart;
    i = hostStart + (url.username !== '' || url.password !== '' ? 1 : 0);
    const hostEnd = i + usm.serializeHost(url.host).length;
    c[3] = hostEnd;
    if (url.port !== null) {
      c[4] = url.port;
      c[5] = hostEnd + 1 + String(url.port).length;
    } else {
      c[4] = omitted;
      c[5] = hostEnd;
    }
  } else {
    c[1] = protocolEnd;
    c[2] = protocolEnd;
    c[3] = protocolEnd;
    c[4] = omitted;
    // ("/." keeps a path that starts with "//" from reading as a host)
    c[5] = !usm.hasAnOpaquePath(url) && url.path.length > 1 && url.path[0] === '' ? protocolEnd + 2 : protocolEnd;
  }
  let end = href.length;
  if (url.fragment !== null) {
    end -= url.fragment.length + 1;
    c[7] = end;
  } else {
    c[7] = omitted;
  }
  c[6] = url.query !== null ? end - url.query.length - 1 : omitted;
  c[8] = schemeTypes[url.scheme] ?? 1;
  return href;
}

function parseBase(base) {
  if (base === undefined) return null;
  return usm.basicURLParse(base);
}

function parse(input, base, raiseException) {
  const parsedBase = parseBase(base);
  const url = base !== undefined && parsedBase === null ? null : usm.basicURLParse(input, { baseURL: parsedBase });
  if (url === null) {
    if (raiseException) throw invalid(input, base);
    return undefined;
  }
  return serialize(url);
}

function canParse(input, base) {
  const parsedBase = parseBase(base);
  if (base !== undefined && parsedBase === null) return false;
  return usm.basicURLParse(input, { baseURL: parsedBase }) !== null;
}

// Node.js's path-to-file-URL escaping: everything but letters, digits and /;=:@&$,+!*'()._-
function escapePath(path) {
  let out = '';
  let bytes;
  for (const ch of path) {
    if (/^[A-Za-z0-9/;=:@&$,+!*'()._-]$/.test(ch)) {
      out += ch;
      continue;
    }
    bytes ??= new TextEncoder();
    for (const b of bytes.encode(ch)) out += '%' + (b < 16 ? '0' : '') + b.toString(16).toUpperCase();
  }
  return out;
}

function pathToFileURL(path, windows, hostname) {
  let p = escapePath(windows ? path.replace(/\\/g, '/') : path);
  if (windows && !p.startsWith('/')) p = '/' + p;
  const url = usm.basicURLParse(`file://${hostname ?? ''}${p}`);
  if (url === null) throw invalid(path);
  return serialize(url);
}

const actions = ['protocol', 'host', 'hostname', 'port', 'username', 'password', 'pathname', 'search', 'hash', 'href'];

// a setter of the URL Standard on the URL `href`: the new href, or false when an href doesn't parse
function update(href, action, value) {
  if (actions[action] === 'href') {
    const url = usm.basicURLParse(value);
    return url === null ? false : serialize(url);
  }
  const url = usm.basicURLParse(href);
  if (url === null) return false;
  switch (actions[action]) {
    case 'protocol':
      usm.basicURLParse(`${value}:`, { url, stateOverride: 'scheme start' });
      break;
    case 'username':
      if (!usm.cannotHaveAUsernamePasswordPort(url)) usm.setTheUsername(url, value);
      break;
    case 'password':
      if (!usm.cannotHaveAUsernamePasswordPort(url)) usm.setThePassword(url, value);
      break;
    case 'host':
      if (!usm.hasAnOpaquePath(url)) usm.basicURLParse(value, { url, stateOverride: 'host' });
      break;
    case 'hostname':
      if (!usm.hasAnOpaquePath(url)) usm.basicURLParse(value, { url, stateOverride: 'hostname' });
      break;
    case 'port':
      if (usm.cannotHaveAUsernamePasswordPort(url)) break;
      if (value === '') url.port = null;
      else usm.basicURLParse(value, { url, stateOverride: 'port' });
      break;
    case 'pathname':
      if (usm.hasAnOpaquePath(url)) break;
      url.path = [];
      usm.basicURLParse(value, { url, stateOverride: 'path start' });
      break;
    case 'search': {
      if (value === '') {
        url.query = null;
        stripOpaquePathSpaces(url);
        break;
      }
      const input = value[0] === '?' ? value.slice(1) : value;
      url.query = '';
      usm.basicURLParse(input, { url, stateOverride: 'query' });
      break;
    }
    case 'hash': {
      if (value === '') {
        url.fragment = null;
        stripOpaquePathSpaces(url);
        break;
      }
      const input = value[0] === '#' ? value.slice(1) : value;
      url.fragment = '';
      usm.basicURLParse(input, { url, stateOverride: 'fragment' });
      break;
    }
  }
  return serialize(url);
}

// (the URL Standard's "potentially strip trailing spaces from an opaque path")
function stripOpaquePathSpaces(url) {
  if (!usm.hasAnOpaquePath(url) || url.fragment !== null || url.query !== null) return;
  url.path = url.path.replace(/ +$/, '');
}

function domainToASCII(domain) {
  if (domain === '') return '';
  const r = usm.domainToASCII(domain);
  return typeof r === 'string' ? r : '';
}

function domainToUnicode(domain) {
  if (domain === '') return '';
  const r = usm.lazyTr46().toUnicode(domain, {
    checkHyphens: false,
    checkBidi: true,
    checkJoiners: true,
    useSTD3ASCIIRules: false,
    transitionalProcessing: false,
    ignoreInvalidPunycode: false,
  });
  return r.error ? '' : r.domain;
}

function getOrigin(href) {
  const url = usm.basicURLParse(href);
  return url === null ? 'null' : usm.serializeURLOrigin(url);
}

// url.format(URL, options)
function format(href, fragment, unicode, search, auth) {
  const url = usm.basicURLParse(href);
  if (url === null) return href;
  if (!fragment) url.fragment = null;
  if (!search) url.query = null;
  if (!auth) {
    url.username = '';
    url.password = '';
  }
  let out = usm.serializeURL(url);
  if (unicode && url.host !== null && typeof url.host === 'string') {
    const ascii = usm.serializeHost(url.host);
    const uni = domainToUnicode(ascii);
    if (uni && uni !== ascii) {
      const at = out.indexOf(ascii, url.scheme.length + 3);
      if (at >= 0) out = out.slice(0, at) + uni + out.slice(at + ascii.length);
    }
  }
  return out;
}

module.exports = {
  urlComponents,
  parse,
  canParse,
  pathToFileURL,
  update,
  domainToASCII,
  domainToUnicode,
  getOrigin,
  format,
};

// URL, URLSearchParams and the url module: WHATWG parsing, setters, the legacy API
const url = require('url');
const util = require('util');
const urls = ['https://example.com', 'https://user:pass@example.com:1234/foo/bar?baz#quux', 'https://user@h/', 'https://:pw@h/',
  'http://h:80/p', 'file:///a/b', 'file://host/a', 'mailto:x@y', 'blob:https://a/b', 'https://h/?', 'https://h/#', 'sc://h', 'sc://u@h',
  'sc:/p', 'sc:p?q#f', 'http://[::1]:8/', 'http://[2001:db8::0:1]/', 'https://h/a?b', 'sc:', 'sc://', 'sc://h:9', 'http://ExAmPle.COM/%7e/../x',
  'https://h:8080?q', 'javascript:alert(1)', 'data:text/plain,hi', 'http://1.2.3.4/', 'http://0x7f.1/', 'sc:///p', 'sc:/.//p',
  'http://xn--nxasmq6b.com/', 'http://ü.com/', 'https://h/ä?ö#ü', '  http://h/\t\n ', 'http://h/a b?c d#e f', 'HTTP://H/./a/./b/../c'];
for (const u of urls) {
  const p = new URL(u);
  console.log(JSON.stringify([p.href, p.origin, p.protocol, p.username, p.password, p.host, p.hostname, p.port, p.pathname, p.search, p.hash]));
}
for (const [i, b] of [['/x', 'https://h/a/b'], ['../y?q', 'https://h/a/b/c'], ['//other/p', 'https://h/'], ['?q', 'sc:o'], ['#f', 'sc:o'], ['x', 'nope']]) {
  try {
    console.log('base', new URL(i, b).href);
  } catch (e) {
    console.log('base', e.name, e.code, e.message, e.input, e.base);
  }
}
for (const bad of ['', 'x', 'http://', 'http://a b/', 'https://h:99999/', 'http://[::1/', 'http://%41/']) {
  try {
    new URL(bad);
    console.log('parsed', bad);
  } catch (e) {
    console.log('invalid', JSON.stringify(bad), e.code, e.message);
  }
}
console.log('canParse', URL.canParse('a', 'http://x'), URL.canParse('a'), URL.parse('nope'), URL.parse('http://h/x').href);
const s = new URL('https://user:pw@h:8/p?a=1#f');
s.protocol = 'http';
s.username = 'u2';
s.password = '';
s.hostname = 'Other.COM';
s.port = '80';
s.pathname = '/a b/../c';
s.search = 'x=1&y=2';
s.hash = 'top';
console.log('set', s.href);
s.port = '81';
s.host = 'h2:90';
console.log('set', s.href, s.port);
s.searchParams.append('z', '3 4');
s.searchParams.delete('x');
console.log('params', s.href, s.search, [...s.searchParams].join(';'));
s.href = 'ftp://f/';
console.log('href', s.href, s.origin);
try {
  s.href = 'nope';
} catch (e) {
  console.log('href invalid', e.code);
}
const o = new URL('sc:op aque  ?q');
o.search = '';
console.log('opaque', JSON.stringify(o.href));
console.log('inspect', util.inspect(new URL('https://a:b@h:1/p?q#f'), { showHidden: true }));
console.log('file', url.pathToFileURL('/a b/c#d?e%f\\g|ü~').href, url.fileURLToPath('file:///a%20b/c'), url.fileURLToPath(new URL('file:///x/y')));
console.log('domain', url.domainToASCII('ü.com'), JSON.stringify(url.domainToASCII('xn--iñvalid.com')), url.domainToUnicode('xn--tda.com'), url.domainToASCII('EXAMPLE.com'));
console.log('format', url.format(new URL('https://a:b@ü.com/p?q#f'), { fragment: false, unicode: true, search: false, auth: false }), url.format(new URL('https://a:b@ü.com/p?q#f')));
const legacy = url.parse('http://user:pw@Host.com:8080/p/a/t/h?query=string#hash', true);
console.log('legacy', legacy.protocol, legacy.auth, legacy.host, legacy.port, legacy.pathname, JSON.stringify(legacy.query), legacy.hash, url.format(legacy));
console.log('resolve', url.resolve('http://a/b/c', '../d'), url.resolve('/one/two', 'three'));
console.log('toJSON', JSON.stringify({ u: new URL('http://h/') }), String(new URL('http://h/x')));

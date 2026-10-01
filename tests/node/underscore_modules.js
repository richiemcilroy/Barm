// Node.js's underscore modules (old names packages still require)
const names = ['_http_agent', '_http_client', '_http_common', '_http_incoming', '_http_outgoing', '_http_server', '_tls_common', '_tls_wrap'];
for (const n of names) {
  const m = require(n);
  console.log(n, typeof m, Object.keys(m).sort().slice(0, 6).join(','));
}

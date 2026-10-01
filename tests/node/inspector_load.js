// inspector loads (Barm has no inspector to connect to), tls contexts
const inspector = require('inspector');
console.log('inspector', typeof inspector.Session, inspector.url());
const tls = require('tls');
const ctx = tls.createSecureContext({ ciphers: 'HIGH', minVersion: 'TLSv1.2' });
console.log('secure context', typeof ctx.context, typeof tls.connect);

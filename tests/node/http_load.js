// http and https load: methods, status codes, Agent (no sockets used)
const http = require('http');
const https = require('https');
console.log('methods', http.METHODS.length, http.METHODS.slice(0, 5).join(','), http.STATUS_CODES[404], http.maxHeaderSize);
const agent = new http.Agent({ keepAlive: true, maxSockets: 5 });
console.log('agent', agent.keepAlive, agent.maxSockets, agent instanceof require('events'), typeof http.globalAgent, https.globalAgent.defaultPort);
console.log('validate', (() => { try { http.validateHeaderName('bad name'); } catch (e) { return e.code; } })());
console.log('classes', typeof http.Server, typeof http.ClientRequest, typeof http.IncomingMessage, typeof http.OutgoingMessage, typeof http.ServerResponse);

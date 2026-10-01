'use strict';

// internal/deps/undici/undici: in Node.js, the bundled undici (fetch, WebSocket, MessageEvent).
// Barm's fetch is its own; this provides what the rest of lib/ takes from undici: MessageEvent.

const { Event } = require('internal/event_target');

class MessageEvent extends Event {
  #data;
  #origin;
  #lastEventId;
  #source;
  #ports;

  constructor(type, init = {}) {
    super(type, init);
    this.#data = init?.data ?? null;
    this.#origin = init?.origin ?? '';
    this.#lastEventId = init?.lastEventId ?? '';
    this.#source = init?.source ?? null;
    this.#ports = Object.freeze([...(init?.ports ?? [])]);
  }

  get data() { return this.#data; }
  get origin() { return this.#origin; }
  get lastEventId() { return this.#lastEventId; }
  get source() { return this.#source; }
  get ports() { return this.#ports; }

  initMessageEvent(type, bubbles = false, cancelable = false, data = null, origin = '', lastEventId = '', source = null, ports = []) {
    return new MessageEvent(type, { bubbles, cancelable, data, origin, lastEventId, source, ports });
  }
}

Object.defineProperty(MessageEvent.prototype, Symbol.toStringTag, { value: 'MessageEvent', configurable: true });

function createFastMessageEvent(type, init) {
  return new MessageEvent(type, init);
}

module.exports = { MessageEvent, createFastMessageEvent };

'use strict';

const { primordials, internalBinding } = require("internal/bootstrap");

const {
  Symbol,
} = primordials;

const kFirstEventParam = Symbol('kFirstEventParam');

module.exports = {
  kFirstEventParam,
};

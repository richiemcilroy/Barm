'use strict';

// internalBinding('trace_events'): Barm records no trace events; every category is off.
module.exports = {
  trace() {},
  isTraceCategoryEnabled: () => false,
  getCategoryEnabledBuffer: () => new Uint8Array(1),
  setTraceCategoryStateUpdateHandler() {},
  getEnabledCategories: () => '',
  setLevel() {},
  CategorySet: class CategorySet { enable() {} disable() {} },
};

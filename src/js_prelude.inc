// JavaScript run once per page before its own scripts: the parts of the
// standard library that are clearer written in JavaScript than as C++
// natives. Included by js_engine.cpp as one raw string literal; keep it
// ES5-plain (it runs on the engine it extends) and under 64 KiB.
R"JSPRELUDE(
(function (global) {
  'use strict';
  var resolveUrl = global.__mepResolveUrl;
  delete global.__mepResolveUrl;
  function hide(target, name, value) {
    Object.defineProperty(target, name, { value: value, enumerable: false, writable: true, configurable: true });
  }

  // ---- Array.prototype: what the native member table doesn't carry ----
  var AP = Array.prototype;
  hide(AP, 'lastIndexOf', function (item, from) {
    var i = from === undefined ? this.length - 1 : Math.min(from < 0 ? this.length + from : from, this.length - 1);
    for (; i >= 0; i--) if (this[i] === item) return i;
    return -1;
  });
  hide(AP, 'fill', function (value, start, end) {
    var len = this.length;
    var s = start === undefined ? 0 : start < 0 ? Math.max(len + start, 0) : Math.min(start, len);
    var e = end === undefined ? len : end < 0 ? Math.max(len + end, 0) : Math.min(end, len);
    for (var i = s; i < e; i++) this[i] = value;
    return this;
  });
  hide(AP, 'at', function (index) { var i = index < 0 ? this.length + index : index; return this[i]; });
  hide(AP, 'findLast', function (fn, self) { for (var i = this.length - 1; i >= 0; i--) if (fn.call(self, this[i], i, this)) return this[i]; return undefined; });
  hide(AP, 'findLastIndex', function (fn, self) { for (var i = this.length - 1; i >= 0; i--) if (fn.call(self, this[i], i, this)) return i; return -1; });
  hide(AP, 'reduceRight', function (fn, initial) {
    var i = this.length - 1, acc;
    if (arguments.length > 1) acc = initial;
    else { if (i < 0) throw new TypeError('Reduce of empty array with no initial value'); acc = this[i--]; }
    for (; i >= 0; i--) acc = fn(acc, this[i], i, this);
    return acc;
  });
  hide(AP, 'copyWithin', function (target, start, end) {
    var len = this.length, e = end === undefined ? len : end, copy = this.slice(start, e);
    for (var i = 0; i < copy.length && target + i < len; i++) this[target + i] = copy[i];
    return this;
  });
  hide(AP, 'toReversed', function () { return this.slice().reverse(); });
  hide(AP, 'toSorted', function (cmp) { return this.slice().sort(cmp); });

  // ---- String.prototype extras ----
  var SP = String.prototype;
  hide(SP, 'matchAll', function (re) {
    var out = [], flags = re.flags.indexOf('g') < 0 ? re.flags + 'g' : re.flags, copy = new RegExp(re.source, flags), m;
    while ((m = copy.exec(this)) !== null) out.push(m);
    return out[Symbol.iterator]();
  });

  // ---- typed arrays: value-coercing array builders ----
  function typed(name, bytes, coerce) {
    function TypedArray(source, byteOffset, length) {
      var items = [], i;
      if (typeof source === 'number') for (i = 0; i < source; i++) items.push(0);
      else if (source && source.__mepBuffer) { var n = length === undefined ? Math.floor(source.byteLength / bytes) : length; for (i = 0; i < n; i++) items.push(0); }
      else if (source) { var list = Array.from(source); for (i = 0; i < list.length; i++) items.push(coerce(list[i])); }
      hide(items, 'BYTES_PER_ELEMENT', bytes);
      hide(items, 'byteLength', items.length * bytes);
      hide(items, 'byteOffset', 0);
      hide(items, 'buffer', { __mepBuffer: true, byteLength: items.length * bytes });
      hide(items, 'set', function (from, offset) { var o = offset || 0, l = Array.from(from); for (var k = 0; k < l.length; k++) items[o + k] = coerce(l[k]); });
      hide(items, 'subarray', function (s, e) { return new TypedArray(items.slice(s, e)); });
      hide(items, '__mepTyped', name);
      return items;
    }
    TypedArray.BYTES_PER_ELEMENT = bytes;
    TypedArray.from = function (source, map) { return new TypedArray(map ? Array.from(source, map) : source); };
    TypedArray.of = function () { return new TypedArray(Array.prototype.slice.call(arguments)); };
    global[name] = TypedArray;
  }
  function wrapInt(bits, signed) {
    var size = Math.pow(2, bits), half = size / 2;
    return function (v) { var n = Math.trunc(Number(v)) || 0; n = ((n % size) + size) % size; return signed && n >= half ? n - size : n; };
  }
  typed('Int8Array', 1, wrapInt(8, true));
  typed('Uint8Array', 1, wrapInt(8, false));
  typed('Uint8ClampedArray', 1, function (v) { var n = Math.round(Number(v)) || 0; return n < 0 ? 0 : n > 255 ? 255 : n; });
  typed('Int16Array', 2, wrapInt(16, true));
  typed('Uint16Array', 2, wrapInt(16, false));
  typed('Int32Array', 4, wrapInt(32, true));
  typed('Uint32Array', 4, wrapInt(32, false));
  typed('Float32Array', 4, function (v) { return Math.fround ? Math.fround(Number(v)) : Number(v); });
  typed('Float64Array', 8, function (v) { return Number(v); });
  global.ArrayBuffer = function ArrayBuffer(length) { this.__mepBuffer = true; this.byteLength = length || 0; };
  global.ArrayBuffer.isView = function (v) { return !!(v && v.__mepTyped); };

  // ---- URLSearchParams / URL ----
  function decode(text) { return decodeURIComponent(String(text).split('+').join(' ')); }
  function encode(text) { return encodeURIComponent(String(text)).split('%20').join('+'); }
  function URLSearchParams(init) {
    var pairs = [];
    hide(this, '__pairs', pairs);
    hide(this, '__owner', null);
    if (typeof init === 'string') {
      var text = init.charAt(0) === '?' ? init.slice(1) : init;
      if (text) text.split('&').forEach(function (part) {
        if (!part) return;
        var eq = part.indexOf('=');
        pairs.push(eq < 0 ? [decode(part), ''] : [decode(part.slice(0, eq)), decode(part.slice(eq + 1))]);
      });
    } else if (init && typeof init.forEach === 'function' && !Array.isArray(init)) {
      init.forEach(function (value, key) { pairs.push([String(key), String(value)]); });
    } else if (Array.isArray(init)) {
      init.forEach(function (pair) { pairs.push([String(pair[0]), String(pair[1])]); });
    } else if (init && typeof init === 'object') {
      Object.keys(init).forEach(function (key) { pairs.push([key, String(init[key])]); });
    }
  }
  var UP = URLSearchParams.prototype;
  function changed(self) { if (self.__owner) self.__owner.__syncSearch(); }
  UP.get = function (name) { for (var i = 0; i < this.__pairs.length; i++) if (this.__pairs[i][0] === name) return this.__pairs[i][1]; return null; };
  UP.getAll = function (name) { return this.__pairs.filter(function (p) { return p[0] === name; }).map(function (p) { return p[1]; }); };
  UP.has = function (name) { return this.__pairs.some(function (p) { return p[0] === name; }); };
  UP.append = function (name, value) { this.__pairs.push([String(name), String(value)]); changed(this); };
  UP.set = function (name, value) {
    var done = false, kept = [];
    this.__pairs.forEach(function (p) { if (p[0] !== name) kept.push(p); else if (!done) { kept.push([p[0], String(value)]); done = true; } });
    if (!done) kept.push([String(name), String(value)]);
    this.__pairs.length = 0;
    for (var i = 0; i < kept.length; i++) this.__pairs.push(kept[i]);
    changed(this);
  };
  UP['delete'] = function (name) {
    var kept = this.__pairs.filter(function (p) { return p[0] !== name; });
    this.__pairs.length = 0;
    for (var i = 0; i < kept.length; i++) this.__pairs.push(kept[i]);
    changed(this);
  };
  UP.forEach = function (fn, self) { var me = this; this.__pairs.slice().forEach(function (p) { fn.call(self, p[1], p[0], me); }); };
  UP.keys = function () { return this.__pairs.map(function (p) { return p[0]; })[Symbol.iterator](); };
  UP.values = function () { return this.__pairs.map(function (p) { return p[1]; })[Symbol.iterator](); };
  UP.entries = function () { return this.__pairs.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); };
  UP[Symbol.iterator] = UP.entries;
  UP.sort = function () { this.__pairs.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); changed(this); };
  UP.toString = function () { return this.__pairs.map(function (p) { return encode(p[0]) + '=' + encode(p[1]); }).join('&'); };
  Object.defineProperty(UP, 'size', { get: function () { return this.__pairs.length; } });
  global.URLSearchParams = URLSearchParams;

  function URL(input, base) {
    var href = resolveUrl(base === undefined ? '' : String(base), String(input));
    var scheme = href.indexOf('://');
    if (scheme < 0) throw new TypeError("Failed to construct 'URL': Invalid URL");
    this.__parse(href);
  }
  URL.prototype.__parse = function (href) {
    var scheme = href.indexOf('://');
    var rest = href.slice(scheme + 3);
    var hashAt = rest.indexOf('#');
    this.hash = hashAt < 0 ? '' : rest.slice(hashAt);
    if (this.hash === '#') this.hash = '';
    if (hashAt >= 0) rest = rest.slice(0, hashAt);
    var queryAt = rest.indexOf('?');
    this.search = queryAt < 0 ? '' : rest.slice(queryAt);
    if (this.search === '?') this.search = '';
    if (queryAt >= 0) rest = rest.slice(0, queryAt);
    var slash = rest.indexOf('/');
    this.host = slash < 0 ? rest : rest.slice(0, slash);
    this.pathname = slash < 0 ? '/' : rest.slice(slash);
    this.protocol = href.slice(0, scheme + 1);
    var colon = this.host.lastIndexOf(':');
    this.hostname = colon < 0 ? this.host : this.host.slice(0, colon);
    this.port = colon < 0 ? '' : this.host.slice(colon + 1);
    this.origin = this.protocol === 'file:' ? 'null' : this.protocol + '//' + this.host;
    this.username = '';
    this.password = '';
    var params = new URLSearchParams(this.search);
    params.__owner = this;
    hide(this, 'searchParams', params);
    this.href = this.toString();
  };
  hide(URL.prototype, '__syncSearch', function () {
    var text = this.searchParams.toString();
    this.search = text ? '?' + text : '';
    this.href = this.toString();
  });
  URL.prototype.toString = function () { return this.protocol + '//' + this.host + this.pathname + this.search + this.hash; };
  URL.prototype.toJSON = URL.prototype.toString;
  URL.canParse = function (input, base) { try { new URL(input, base); return true; } catch (e) { return false; } };
  global.URL = URL;

  // ---- small globals ----
  global.structuredClone = function (value) { return value === undefined ? undefined : JSON.parse(JSON.stringify(value)); };
  if (!Promise.any) Promise.any = function (list) {
    return new Promise(function (resolve, reject) {
      var items = Array.from(list), errors = [], left = items.length;
      if (!left) reject(new Error('All promises were rejected'));
      items.forEach(function (item, i) { Promise.resolve(item).then(resolve, function (e) { errors[i] = e; if (--left === 0) reject(new Error('All promises were rejected')); }); });
    });
  };
})(this);
)JSPRELUDE"

// The grammar DSL `grammar.js` files are written in -- grammar(), seq(),
// choice(), prec(), token(), ... -- for mep-ts-generate, which runs it on
// mep's own JS engine instead of Node. Its output is the object tree-sitter
// 0.25.10's CLI writes to src/grammar.json, key for key: the generator's
// byte-identical check covers this file too (`just grammar-generate-check`).
//
// grammar_js.cpp prepends this to the grammar's modules, each wrapped as
//   __mepDefine(path, {"./dep.js": path, ...}, function (module, exports, require) {...})
// (a JSON module passes its value instead of a function), then calls
// __mepGrammarJson(path) and prints what it returns.

// A `$.name` for a rule the grammar doesn't define: an error once used as
// a rule, but fine as an alias's name (alias($.x, $.new_name)).
function UndefinedSymbol(name) {
  this.symbol = sym(name);
  this.message = "Undefined symbol '" + name + "'";
}

function isRegExp(value) {
  return value instanceof RegExp;
}

function normalize(value) {
  if (value === undefined) throw new Error('Undefined symbol');
  if (value === null) throw new TypeError('Invalid rule: null');
  if (typeof value === 'string') return { type: 'STRING', value: value };
  if (isRegExp(value)) {
    return value.flags ? { type: 'PATTERN', value: value.source, flags: value.flags } : { type: 'PATTERN', value: value.source };
  }
  if (value instanceof UndefinedSymbol) throw new ReferenceError(value.message);
  if (typeof value.type === 'string') return value;
  throw new TypeError('Invalid rule: ' + value);
}

function alias(rule, value) {
  const result = { type: 'ALIAS', content: normalize(rule), named: false, value: null };
  if (typeof value === 'string') {
    result.value = value;
    return result;
  }
  if (value instanceof UndefinedSymbol) value = value.symbol;
  if (value && value.type === 'SYMBOL') {
    result.named = true;
    result.value = value.name;
    return result;
  }
  throw new Error('Invalid alias value ' + value);
}

function blank() {
  return { type: 'BLANK' };
}

function field(name, rule) {
  return { type: 'FIELD', name: name, content: normalize(rule) };
}

function choice(...elements) {
  return { type: 'CHOICE', members: elements.map(normalize) };
}

function optional(value) {
  return choice(value, blank());
}

function checkPrecedence(value) {
  if (value === undefined) throw new Error('Missing precedence value');
  if (typeof value !== 'number' && typeof value !== 'string') throw new Error('Precedence must be a number or a string');
}

function prec(number, rule) {
  checkPrecedence(number);
  if (rule === undefined) throw new Error('Missing rule in prec()');
  return { type: 'PREC', value: number, content: normalize(rule) };
}

prec.left = function (number, rule) {
  if (rule === undefined) {
    rule = number;
    number = 0;
  }
  checkPrecedence(number);
  return { type: 'PREC_LEFT', value: number, content: normalize(rule) };
};

prec.right = function (number, rule) {
  if (rule === undefined) {
    rule = number;
    number = 0;
  }
  checkPrecedence(number);
  return { type: 'PREC_RIGHT', value: number, content: normalize(rule) };
};

prec.dynamic = function (number, rule) {
  if (typeof number !== 'number') throw new Error('Dynamic precedence must be a number');
  return { type: 'PREC_DYNAMIC', value: number, content: normalize(rule) };
};

function repeat(rule) {
  return { type: 'REPEAT', content: normalize(rule) };
}

function repeat1(rule) {
  return { type: 'REPEAT1', content: normalize(rule) };
}

function seq(...elements) {
  return { type: 'SEQ', members: elements.map(normalize) };
}

function sym(name) {
  return { type: 'SYMBOL', name: name };
}

function token(value) {
  return { type: 'TOKEN', content: normalize(value) };
}

token.immediate = function (value) {
  return { type: 'IMMEDIATE_TOKEN', content: normalize(value) };
};

function reserved(wordset, rule) {
  if (typeof wordset !== 'string') throw new Error('Invalid reserved word set name: ' + wordset);
  return { type: 'RESERVED', content: normalize(rule), context_name: wordset };
}

// `$`: $.name is a symbol, or an UndefinedSymbol when `known` lacks it.
function ruleBuilder(known) {
  return new Proxy({}, {
    get(_, name) {
      if (typeof name !== 'string') return undefined;
      if (!known || known[name]) return sym(name);
      return new UndefinedSymbol(name);
    },
  });
}

function grammar(baseGrammar, options) {
  let inherits;
  if (!options) {
    options = baseGrammar;
    baseGrammar = {
      name: null, rules: {}, extras: [normalize(/\s/)], conflicts: [], externals: [], inline: [], supertypes: [],
      precedences: [], reserved: {},
    };
  } else {
    baseGrammar = baseGrammar.grammar;
    inherits = baseGrammar.name;
  }
  if (typeof options.name !== 'string') throw new Error('Grammar must have a name');
  if (!/^[a-zA-Z_]\w*$/.test(options.name)) throw new Error('Grammar name must be a valid identifier');

  let externals = baseGrammar.externals;
  if (options.externals) {
    if (typeof options.externals !== 'function') throw new Error("Grammar's 'externals' property must be a function");
    const $ = ruleBuilder(null);
    externals = options.externals.call($, $, baseGrammar.externals).map(normalize);
  }

  const known = {};
  for (const name of Object.keys(options.rules || {})) known[name] = true;
  for (const name of Object.keys(baseGrammar.rules)) known[name] = true;
  for (const external of externals) if (typeof external.name === 'string') known[external.name] = true;
  const $ = ruleBuilder(known);

  const name = options.name;
  const rules = Object.assign({}, baseGrammar.rules);
  if (options.rules) {
    if (typeof options.rules !== 'object') throw new Error("Grammar's 'rules' property must be an object");
    for (const ruleName of Object.keys(options.rules)) {
      const ruleFn = options.rules[ruleName];
      if (typeof ruleFn !== 'function') throw new Error("Grammar rules must all be functions. '" + ruleName + "' rule is not");
      const rule = ruleFn.call($, $, baseGrammar.rules[ruleName]);
      if (rule === undefined) throw new Error("Rule '" + ruleName + "' returned undefined");
      rules[ruleName] = normalize(rule);
    }
  }

  let reservedWords = baseGrammar.reserved;
  if (options.reserved) {
    if (typeof options.reserved !== 'object') throw new Error("Grammar's 'reserved' property must be an object");
    reservedWords = {};
    for (const wordset of Object.keys(options.reserved)) {
      const fn = options.reserved[wordset];
      if (typeof fn !== 'function') throw new Error("Grammar reserved word sets must all be functions. '" + wordset + "' is not");
      const words = fn.call($, $, baseGrammar.reserved[wordset]);
      if (!Array.isArray(words)) throw new Error("Grammar's reserved word set functions must all return arrays of rules. '" + wordset + "' does not");
      reservedWords[wordset] = words.map(normalize);
    }
  }

  let extras = baseGrammar.extras.slice();
  if (options.extras) {
    if (typeof options.extras !== 'function') throw new Error("Grammar's 'extras' property must be a function");
    extras = options.extras.call($, $, baseGrammar.extras).map(normalize);
  }

  let word = baseGrammar.word;
  if (options.word) {
    word = options.word.call($, $).name;
    if (typeof word !== 'string') throw new Error("Grammar's 'word' property must be a named rule");
    if (word === 'ReferenceError') throw new Error("Grammar's 'word' property must be a valid rule name");
  }

  let conflicts = baseGrammar.conflicts;
  if (options.conflicts) {
    if (typeof options.conflicts !== 'function') throw new Error("Grammar's 'conflicts' property must be a function");
    const base = baseGrammar.conflicts.map(set => set.map(sym));
    const sets = options.conflicts.call($, $, base);
    if (!Array.isArray(sets)) throw new Error("Grammar's conflicts must be an array of arrays of rules");
    conflicts = sets.map(set => {
      if (!Array.isArray(set)) throw new Error("Grammar's conflicts must be an array of arrays of rules");
      return set.map(symbol => normalize(symbol).name);
    });
  }

  let inline = baseGrammar.inline;
  if (options.inline) {
    if (typeof options.inline !== 'function') throw new Error("Grammar's 'inline' property must be a function");
    const rules = options.inline.call($, $, baseGrammar.inline.map(sym));
    if (!Array.isArray(rules)) throw new Error("Grammar's inline must be an array of rules");
    inline = rules.filter(symbol => {
      if (symbol instanceof UndefinedSymbol) return false;
      if (typeof symbol.name !== 'string') throw new Error("Grammar's inline must be an array of rules");
      return true;
    }).map(symbol => symbol.name);
  }

  let supertypes = baseGrammar.supertypes;
  if (options.supertypes) {
    if (typeof options.supertypes !== 'function') throw new Error("Grammar's 'supertypes' property must be a function");
    const rules = options.supertypes.call($, $, baseGrammar.supertypes.map(sym));
    if (!Array.isArray(rules)) throw new Error("Grammar's supertypes must be an array of rules");
    supertypes = rules.map(symbol => {
      if (symbol instanceof UndefinedSymbol) throw new ReferenceError(symbol.message);
      if (typeof symbol.name !== 'string') throw new Error("Grammar's supertypes must be an array of rules");
      return symbol.name;
    });
  }

  let precedences = baseGrammar.precedences;
  if (options.precedences) {
    if (typeof options.precedences !== 'function') throw new Error("Grammar's 'precedences' property must be a function");
    const lists = options.precedences.call($, $, baseGrammar.precedences);
    if (!Array.isArray(lists)) throw new Error("Grammar's precedences must be an array of arrays of rules");
    precedences = lists.map(list => {
      if (!Array.isArray(list)) throw new Error("Grammar's precedences must be an array of arrays of rules");
      return list.map(normalize);
    });
  }

  if (Object.keys(rules).length === 0) throw new Error('Grammar must have at least one rule.');

  return {
    grammar: {
      name, inherits, word, rules, extras, conflicts, precedences, externals, inline, supertypes,
      reserved: reservedWords,
    },
  };
}

// ---- modules ------------------------------------------------------------------

const __mepModules = {};

function __mepDefine(path, deps, body) {
  __mepModules[path] = { deps: deps, body: body, loaded: false, module: { exports: {} } };
}

function __mepRequire(path) {
  const entry = __mepModules[path];
  if (!entry) throw new Error('Cannot find module ' + path);
  if (!entry.loaded) {
    entry.loaded = true;
    if (typeof entry.body === 'function') {
      const require = name => {
        if (!(name in entry.deps)) throw new Error("Cannot find module '" + name + "' from " + path);
        return __mepRequire(entry.deps[name]);
      };
      entry.body.call(entry.module.exports, entry.module, entry.module.exports, require);
    } else {
      entry.module.exports = entry.body;
    }
  }
  return entry.module.exports;
}

function __mepGrammarJson(path) {
  const exported = __mepRequire(path);
  const result = exported && exported.default ? exported.default : exported;
  if (!result || !result.grammar) throw new Error(path + ' does not export a grammar');
  const object = { $schema: 'https://tree-sitter.github.io/tree-sitter/assets/schemas/grammar.schema.json' };
  Object.assign(object, result.grammar);
  return JSON.stringify(object, null, 2);
}

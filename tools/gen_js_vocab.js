#!/usr/bin/env node
// Generate src/js_lsp_vocab.cpp: the names mep-js-lsp knows.
//
//     just js-lsp-vocab        (node tools/gen_js_vocab.js PATH/TO/typescript)
//
// Runs TypeScript's own checker over its bundled lib files (lib.esnext.full:
// ECMAScript plus the DOM) and records every global, the static members of
// every global object (Math.max, JSON.parse, Promise.all, Object.keys) and
// the instance members of the core types (Array.prototype.map,
// String.prototype.trim, ...) with their signatures as TypeScript prints
// them.
//
// Documentation: the ECMAScript lib files (lib.es*.d.ts) are Microsoft's,
// Apache-2.0, and their JSDoc is embedded with the notice. The DOM lib's
// descriptions come from MDN (CC-BY-SA), so DOM names keep their signatures
// but no prose -- a hover links to MDN instead.

const fs = require('fs');
const path = require('path');

const tsdir = process.argv[2];
if (!tsdir) {
  console.error('usage: gen_js_vocab.js PATH/TO/node_modules/typescript');
  process.exit(2);
}
const ts = require(tsdir);
const repo = path.resolve(__dirname, '..');
const out = path.join(repo, 'src', 'js_lsp_vocab.cpp');

const libdir = path.join(tsdir, 'lib');
const entry = path.join(libdir, 'lib.esnext.full.d.ts');
const program = ts.createProgram([entry], { noLib: false, lib: ['lib.esnext.full.d.ts'], target: ts.ScriptTarget.ESNext });
const checker = program.getTypeChecker();

// Instance types whose members are worth offering after a `.`.
const INSTANCE = ['Array', 'ReadonlyArray', 'String', 'Number', 'Boolean', 'BigInt', 'Symbol', 'Function', 'Object', 'Promise',
  'Map', 'Set', 'WeakMap', 'WeakSet', 'WeakRef', 'RegExp', 'Date', 'Error', 'ArrayBuffer', 'DataView', 'Uint8Array',
  'Iterator', 'AsyncIterator', 'Generator', 'AsyncGenerator', 'RegExpMatchArray',
  'Document', 'Element', 'HTMLElement', 'Node', 'EventTarget', 'Event', 'Response', 'Request', 'Headers', 'URL',
  'URLSearchParams', 'Console', 'Storage', 'Window', 'Location', 'History', 'Navigator', 'CanvasRenderingContext2D',
  'NodeList', 'DOMTokenList', 'CSSStyleDeclaration', 'FormData', 'Blob', 'File', 'AbortController', 'AbortSignal',
  'TextEncoder', 'TextDecoder', 'Worker', 'WebSocket', 'XMLHttpRequest'];

function isEsFile(decl) {
  const f = path.basename(decl.getSourceFile().fileName);
  return /^lib\.(es|decorators)/.test(f);
}
function docOf(sym) {
  const decls = sym.getDeclarations() || [];
  if (!decls.length || !decls.every(isEsFile)) return '';
  let doc = ts.displayPartsToString(sym.getDocumentationComment(checker));
  const tags = sym.getJsDocTags(checker)
    .filter(t => t.name === 'param' || t.name === 'returns' || t.name === 'deprecated')
    .map(t => {
      const text = ts.displayPartsToString(t.text || []);
      if (t.name === 'param') {
        const m = text.match(/^(\S+)\s*(.*)$/s);
        return m ? `*@param* \`${m[1]}\` — ${m[2]}` : `*@param* ${text}`;
      }
      return `*@${t.name}* ${text}`;
    });
  if (tags.length) doc += (doc ? '\n\n' : '') + tags.join('\n\n');
  return doc.trim();
}
function originOf(sym) {
  const decls = sym.getDeclarations() || [];
  if (!decls.length) return 'es';
  return decls.some(isEsFile) ? 'es' : 'dom';
}
function kindOf(sym) {
  const f = sym.getFlags();
  if (f & ts.SymbolFlags.Class) return 'class';
  if (f & ts.SymbolFlags.Function) return 'function';
  if (f & ts.SymbolFlags.Method) return 'method';
  if (f & ts.SymbolFlags.Interface && f & ts.SymbolFlags.Variable) return 'class';  // `interface X` + `declare var X: XConstructor`
  if (f & ts.SymbolFlags.Interface) return 'interface';
  if (f & ts.SymbolFlags.TypeAlias) return 'type';
  if (f & ts.SymbolFlags.Enum) return 'enum';
  if (f & ts.SymbolFlags.Namespace || f & ts.SymbolFlags.NamespaceModule || f & ts.SymbolFlags.ValueModule) return 'namespace';
  if (f & ts.SymbolFlags.Property) return 'property';
  if (f & ts.SymbolFlags.Variable) return 'variable';
  return 'value';
}
function signatureOf(sym, decl) {
  try {
    const type = checker.getTypeOfSymbolAtLocation(sym, decl);
    const sigs = type.getCallSignatures();
    if (sigs.length && (kindOf(sym) === 'function' || kindOf(sym) === 'method' || (sym.getFlags() & ts.SymbolFlags.Property))) {
      // Every overload, one per line.
      return sigs.slice(0, 6).map(s => checker.signatureToString(s, undefined, ts.TypeFormatFlags.WriteArrowStyleSignature ? 0 : 0)).join('\n');
    }
    const t = checker.typeToString(type, undefined, ts.TypeFormatFlags.NoTruncation);
    return t.length > 300 ? t.slice(0, 297) + '...' : t;
  } catch (e) {
    return '';
  }
}

const rows = new Map();  // name -> row
function add(name, sym, decl, origin) {
  if (rows.has(name)) return;
  rows.set(name, { origin: origin || originOf(sym), name, kind: kindOf(sym), sig: signatureOf(sym, decl), doc: docOf(sym) });
}

const sf = program.getSourceFile(entry);
const globals = checker.getSymbolsInScope(sf, ts.SymbolFlags.Value | ts.SymbolFlags.Type | ts.SymbolFlags.Namespace);
const declOf = sym => (sym.getDeclarations() || [])[0];
for (const sym of globals) {
  const name = sym.getName();
  if (name.startsWith('__') || name === 'globalThis') continue;
  const decl = declOf(sym);
  if (!decl) continue;
  if (!(sym.getFlags() & (ts.SymbolFlags.Value | ts.SymbolFlags.Namespace))) continue;  // types alone are TS-only; see below
  add(name, sym, decl);
  // Static members: the members of the global value's type (Math.max, Promise.all, Object.keys).
  if (sym.getFlags() & (ts.SymbolFlags.Variable | ts.SymbolFlags.Namespace)) {
    const type = checker.getTypeOfSymbolAtLocation(sym, decl);
    for (const m of checker.getPropertiesOfType(type)) {
      const md = declOf(m);
      if (!md || m.getName().startsWith('__')) continue;
      if (m.getName() === 'prototype') continue;
      add(name + '.' + m.getName(), m, md);
    }
  }
}
// Types, for TypeScript: interfaces and type aliases with no value.
for (const sym of globals) {
  const f = sym.getFlags();
  const decl = declOf(sym);
  if (!decl || rows.has(sym.getName()) || sym.getName().startsWith('__')) continue;
  if (f & (ts.SymbolFlags.Interface | ts.SymbolFlags.TypeAlias)) {
    rows.set(sym.getName(), { origin: originOf(sym), name: sym.getName(), kind: kindOf(sym), sig: '', doc: docOf(sym) });
  }
}
// Instance members of the core types.
for (const iname of INSTANCE) {
  const isym = globals.find(s => s.getName() === iname && s.getFlags() & ts.SymbolFlags.Interface);
  if (!isym) continue;
  const type = checker.getDeclaredTypeOfSymbol(isym);
  for (const m of checker.getPropertiesOfType(type)) {
    const md = declOf(m);
    if (!md || m.getName().startsWith('__')) continue;
    add(iname + '.prototype.' + m.getName(), m, md);
  }
}

// Node.js's globals (no @types/node here): names only.
const NODE = ['require', 'module', 'exports', 'process', 'Buffer', '__dirname', '__filename', 'global', 'setImmediate',
  'clearImmediate'];
for (const n of NODE) if (!rows.has(n)) rows.set(n, { origin: 'node', name: n, kind: n === 'require' || n.endsWith('Immediate') ? 'function' : 'variable', sig: '', doc: '' });

function cstr(s) {
  let o = '';
  for (const ch of s) {
    const c = ch.codePointAt(0);
    if (ch === '\\') o += '\\\\';
    else if (ch === '"') o += '\\"';
    else if (ch === '\n') o += '\\n';
    else if (ch === '?') o += '\\?';
    else if (c < 32) o += '\\' + c.toString(8).padStart(3, '0');
    else o += ch;
  }
  return '"' + o + '"';
}

const sorted = [...rows.values()].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
const lines = [
  '// Generated by tools/gen_js_vocab.js -- do not edit.',
  `// From TypeScript ${ts.version}'s lib files. The ECMAScript declarations and their`,
  '// documentation (lib.es*.d.ts): Copyright (c) Microsoft Corporation, Apache License 2.0.',
  "// DOM names and signatures only (the DOM lib's prose is MDN's, CC-BY-SA).",
  '#include "js_lsp_vocab.h"',
  '',
  'namespace {',
  '',
  'const JsLspApi kApi[] = {',
];
// Split very long strings so no single literal is huge.
for (const r of sorted) {
  lines.push(`    {${cstr(r.origin)}, ${cstr(r.name)}, ${cstr(r.kind)}, ${cstr(r.sig)}, ${cstr(r.doc.slice(0, 2500))}},`);
}
lines.push('};', '', '}  // namespace', '', 'const JsLspApi *JsLspApiTable(size_t *count) {', '    *count = sizeof(kApi) / sizeof(*kApi);', '    return kApi;', '}', '');
fs.writeFileSync(out, lines.join('\n'));
const counts = {};
for (const r of sorted) counts[r.origin] = (counts[r.origin] || 0) + 1;
console.error(`${sorted.length} names -> src/js_lsp_vocab.cpp`, JSON.stringify(counts));

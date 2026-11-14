// JavaScript (Node.js) language UI mode (<leader>uu / <leader>lu /
// :MepLanguageUi in a .js / .mjs / .cjs buffer), built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) like the Julia and Lua
// modes, whose shape it follows. Included by main.cpp only;
// BuiltinLanguageUiJavascript() is the Lua chunk (help/javascript-mode.org
// is the user's page).
//
// The console is the stock `node` REPL -- `node mep_init.js`
// (mep.opt.javascript_ui_cmd, default {'node'}; a `// REPL:` comment
// replaces it via mep.language_ui_cmd) -- started by an init script that
// calls repl.start() with the stock evaluator, so line editing, completion,
// `.editor`, top-level await and value display are Node's own. It inherits
// mep's environment: NODE_PATH from the dev shell, so `require('pkg')`
// loads what it would in a terminal in that project (in datamunge,
// `@datamunge/datamungejs`, the Rosetta-generated addon).
//
// The init script (mep_javascript_ui_init_template below) wraps the REPL's
// eval: every input -- typed or sent -- is rewritten (top-level
// `const`/`let`/`class` become globals that can be re-declared, static
// `import` becomes `await import()`, `export` is dropped), evaluated by the
// stock evaluator, and then the panel files are rewritten. Lines starting
// with `:` are the mode's own commands (:help, :inspect, :eval, :run).
// Nothing reads the console's output; the panels are files the poll loop
// re-reads on a timer.
//   - Variables: the globals the session has made, with kind and value.
//     Clicking a row inspects it.
//   - Modules: what the session require()d or import()ed, with version and
//     file. Clicking one shows its documentation in Help.
//   - History: what was evaluated, newest last; sent code with its lines.
//   - Inspect: `:inspect x` -- util.inspect of the value, a few levels deep.
//   - Help: `:help name` -- the JSDoc a package's TypeScript declarations
//     (`types` / `typings` in its package.json, or the .d.ts beside its
//     main) give the name, read by `mep-js-lsp --dts-doc` (the language
//     server's own .d.ts reader) or, when that binary predates the flag, a
//     small reader in the init script; then the runtime: a function's
//     signature from its source and arity, a class's methods, an object's
//     keys, a Node built-in module's exports.
//
// Sending code. <leader>re and <leader>rr never paste into the prompt: they
// write the code to a file in the session directory, padded so its line
// numbers are the source's, and type one `:eval "file" "source"` line,
// which evaluates it under the source file's name with `require`,
// `__filename` and `__dirname` relative to that file -- stack traces point
// at the real file and line. The block at the cursor is a top-level
// statement: from the nearest line at or above it that starts one at
// column 0, outside any bracket, to just before the next (a whole function
// or class, an object literal, a chained call).
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiJavascript() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.javascript_ui_cmd = mep.opt.javascript_ui_cmd or {'node'}
mep.opt.javascript_ui_info_share = mep.opt.javascript_ui_info_share or 0.3
mep.opt.javascript_ui_bottom_share = mep.opt.javascript_ui_bottom_share or 0.4
mep.opt.javascript_ui_bottom_info_share = mep.opt.javascript_ui_bottom_info_share or 0.4
for _, ext in ipairs({'js', 'mjs', 'cjs'}) do
  mep.language_ui_extensions[ext] = mep.language_ui_extensions[ext] or 'javascript'
end

-- The console's init script. Lua puts two lines above this: the session
-- directory (MEP_DIR) and the mep-js-lsp binary (MEP_JS_LSP).
local mep_javascript_ui_init_template = [==[
'use strict';
const fs = require('fs');
const path = require('path');
const util = require('util');
const repl = require('repl');
const Module = require('module');
const childProcess = require('child_process');

const own = new Set(['_', '_error', '__mep', 'require', 'module', 'exports', '__filename', '__dirname', '__default']);
const history = [];
const loaded = new Map();        // specifier or path -> {name, file, version, exports}
const sessionFiles = new Set();  // files :eval / :run ran (their require()s are the session's)
let lspUsable = Boolean(MEP_JS_LSP);
let baseGlobals = new Set();

function writeText(name, text) {
  try { fs.writeFileSync(path.join(MEP_DIR, name), text); } catch (e) { /* the panels are best effort */ }
}

function oneLine(s, width) {
  s = String(s).replace(/\s*[\r\n]+\s*/g, ' ');
  return s.length > width ? s.slice(0, width - 3) + '...' : s;
}

function pad(s, n) { s = String(s); return s.length >= n ? s : s + ' '.repeat(n - s.length); }

// ---------------------------------------------------------------------------------------------------------------
// Rewriting what is evaluated

// Top-level `const` / `let` / `class` become `var` (globals the panels can list and a second send can
// re-declare), static `import` becomes `await import()`, `export` is dropped. Newlines are kept, so line
// numbers stay the source's. Strings, templates, comments and regular expressions are skipped.
function transform(code) {
  const edits = [];
  const n = code.length;
  let i = 0, depth = 0;
  let prev = '';            // last significant character outside strings and comments
  let prevWord = '';        // the identifier that character ended, if any
  let newline = true;       // a newline since prev
  let classAt = -1;         // depth-0 class whose body is open: insert ';' after its '}'
  let classPending = false;
  const templates = [];     // depth at each open `${`
  const isId = c => /[\w$]/.test(c);
  const regexBefore = () => prev === '' || '(,=:[!&|?{};+-*%<>~^'.includes(prev) ||
    ['return', 'typeof', 'instanceof', 'in', 'of', 'new', 'delete', 'void', 'throw', 'case', 'do', 'else', 'yield', 'await'].includes(prevWord);
  const stmtStart = () => depth === 0 && templates.length === 0 &&
    (prev === '' || prev === ';' || prev === '}' || (newline && !',=+-*/%&|^!<>?:.([{'.includes(prev)));
  function skipString(q) {
    i++;
    while (i < n && code[i] !== q) {
      if (code[i] === '\\') i++;
      else if (code[i] === '\n') break;
      i++;
    }
    i++;
  }
  // Scans template text from i (after ` or a closing `}` of ${...}); stops after the closing ` or ${.
  function skipTemplate() {
    while (i < n) {
      const c = code[i];
      if (c === '\\') { i += 2; continue; }
      if (c === '`') { i++; return; }
      if (c === '$' && code[i + 1] === '{') { i += 2; templates.push(depth); depth++; return; }
      i++;
    }
  }
  function blankOut(from, to) {
    edits.push([from, to, code.slice(from, to).replace(/[^\n]/g, ' ')]);
  }
  while (i < n) {
    const c = code[i];
    if (c === '\n') { newline = true; i++; continue; }
    if (/\s/.test(c)) { i++; continue; }
    if (c === '/' && code[i + 1] === '/') { while (i < n && code[i] !== '\n') i++; continue; }
    if (c === '/' && code[i + 1] === '*') { const e = code.indexOf('*/', i + 2); i = e < 0 ? n : e + 2; continue; }
    if (c === '"' || c === "'") { skipString(c); prev = c; prevWord = ''; newline = false; continue; }
    if (c === '`') { i++; skipTemplate(); prev = '`'; prevWord = ''; newline = false; continue; }
    if (c === '/' && regexBefore()) {
      i++;
      let inClass = false;
      while (i < n && code[i] !== '\n') {
        if (code[i] === '\\') i++;
        else if (code[i] === '[') inClass = true;
        else if (code[i] === ']') inClass = false;
        else if (code[i] === '/' && !inClass) break;
        i++;
      }
      i++;
      while (i < n && /[a-z]/.test(code[i])) i++;
      prev = '/'; prevWord = ''; newline = false;
      continue;
    }
    if (isId(c)) {
      let j = i;
      while (j < n && isId(code[j])) j++;
      const word = code.slice(i, j);
      if (stmtStart() && !/^\d/.test(word)) {
        const rest = code.slice(j);
        if ((word === 'const' || word === 'let') && /^\s+[\w$[{]/.test(rest)) {
          edits.push([i, j, 'var']);
        } else if (word === 'class' && /^\s+[\w$]/.test(rest)) {
          const name = /^\s+([\w$]+)/.exec(rest)[1];
          edits.push([i, i, 'var ' + name + ' = ']);
          classPending = true;
        } else if (word === 'import' && !/^\s*[(.]/.test(rest)) {
          const m = /^import\s+(?:type\s+)?(?:([\w$]+)\s*(?:,\s*)?)?(?:\*\s*as\s+([\w$]+)|\{([^}]*)\})?\s*(?:from\s*)?(['"])([^'"\n]+)\4(?:\s*(?:with|assert)\s*\{[^}]*\})?\s*;?/.exec(code.slice(i));
          if (m && !/^import\s+type\s/.test(m[0])) {
            const [whole, def, star, named, , spec] = m;
            const load = 'await __mep.import(' + JSON.stringify(spec) + ')';
            let out;
            if (star) out = 'var ' + star + ' = ' + load + (def ? ', ' + def + ' = ' + star + '.default' : '') + ';';
            else if (named !== undefined || def) {
              const fields = [];
              if (def) fields.push('default: ' + def);
              for (const part of (named || '').split(',')) {
                const p = part.trim().replace(/^type\s+/, '');
                if (!p || /^type$/.test(p)) continue;
                const as = /^(.+?)\s+as\s+([\w$]+)$/.exec(p);
                fields.push(as ? as[1] + ': ' + as[2] : p);
              }
              out = 'var {' + fields.join(', ') + '} = __mep.named(' + load + ');';
            } else out = load + ';';
            edits.push([i, i + whole.length, out + whole.replace(/[^\n]/g, '')]);
            i += whole.length; prev = ';'; prevWord = ''; newline = false;
            continue;
          }
        } else if (word === 'export') {
          const m = /^export\s+(default\s+)?/.exec(code.slice(i));
          const after = code.slice(i + m[0].length);
          if (m[1] && !/^(async\s+function|function|class)\s*[\w$]/.test(after.replace(/^(async\s+function|function)\s*\*\s*/, 'function '))) {
            edits.push([i, i + m[0].length, 'var __default = ']);
          } else if (/^(\{|\*)/.test(after)) {
            const end = /^(?:\{[^}]*\}|\*(?:\s*as\s+[\w$]+)?)\s*(?:from\s*(['"])[^'"\n]*\1)?\s*;?/.exec(after);
            const stop = i + m[0].length + (end ? end[0].length : 0);
            blankOut(i, stop);
            i = stop; prev = ';'; prevWord = ''; newline = false;
            continue;
          } else {
            edits.push([i, i + m[0].length, '']);
          }
          i += m[0].length;
          continue;  // the declaration after it is still at the start of a statement
        }
      }
      prev = word[word.length - 1]; prevWord = word; newline = false;
      i = j;
      continue;
    }
    if (c === '(' || c === '[' || c === '{') {
      if (c === '{' && classPending && depth === 0) { classAt = 0; classPending = false; }
      depth++;
    } else if (c === ')' || c === ']' || c === '}') {
      if (c === '}' && templates.length && templates[templates.length - 1] === depth - 1) {
        depth--; templates.pop(); i++; skipTemplate(); prev = '`'; prevWord = ''; newline = false; continue;
      }
      depth = Math.max(0, depth - 1);
      if (c === '}' && depth === 0 && classAt === 0) { edits.push([i + 1, i + 1, ';']); classAt = -1; }
    } else if (c === ';' && depth === 0) {
      classPending = false;
    }
    prev = c; prevWord = ''; newline = false;
    i++;
  }
  let out = code;
  for (const [from, to, text] of edits.sort((a, b) => b[0] - a[0])) out = out.slice(0, from) + text + out.slice(to);
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Panels

function kindOf(v) {
  if (v === null) return 'null';
  if (typeof v === 'function') {
    const src = Function.prototype.toString.call(v);
    return /^class\b/.test(src) ? 'class' : /^async\b/.test(src) ? 'async fn' : 'function';
  }
  if (typeof v !== 'object') return typeof v;
  if (Array.isArray(v)) return 'Array';
  if (Object.prototype.toString.call(v) === '[object Module]') return 'module';
  const proto = Object.getPrototypeOf(v);
  return (proto && proto.constructor && proto.constructor.name) || 'Object';
}

// `name(a, b = 1)` from a function's source; `name(3 args)` for a native one.
function signature(fn, name) {
  name = name || fn.name || '(anonymous)';
  let src = '';
  try { src = Function.prototype.toString.call(fn); } catch (e) { /* a revoked proxy */ }
  if (/^class\b/.test(src)) {
    const m = /\bconstructor\s*(\([^)]*\))/.exec(src);
    return 'class ' + name + (m ? ' ' + oneLine(m[1], 120) : '');
  }
  const m = /^(?:async\s+)?(?:function\s*\*?\s*[\w$]*\s*)?(\([^)]*\)|[\w$]+)\s*(?:=>|\{)/.exec(src) ||
            /^(?:async\s+)?(?:get\s+|set\s+|static\s+)*\*?\s*[\w$]+\s*(\([^)]*\))/.exec(src);
  if (m && !/\[native code\]/.test(src)) return name + (m[1][0] === '(' ? oneLine(m[1], 160) : '(' + m[1] + ')');
  return name + '(' + fn.length + (fn.length === 1 ? ' arg' : ' args') + ')';
}

function detail(v) {
  if (typeof v === 'function') return signature(v);
  try {
    return oneLine(util.inspect(v, {depth: 0, breakLength: Infinity, maxArrayLength: 10, maxStringLength: 60}), 70);
  } catch (e) { return '?'; }
}

function sessionNames() {
  return Object.getOwnPropertyNames(globalThis).filter(n => !baseGlobals.has(n) && !own.has(n)).sort();
}

function refresh() {
  const rows = [];
  for (const name of sessionNames()) {
    const d = Object.getOwnPropertyDescriptor(globalThis, name);
    if (!d) continue;
    if (d.get) { rows.push(pad(name, 22) + ' ' + pad('getter', 10)); continue; }
    rows.push(pad(name, 22) + ' ' + pad(kindOf(d.value), 10) + ' ' + detail(d.value));
  }
  writeText('variables.txt', rows.join('\n') + (rows.length ? '\n' : ''));
  const mods = [];
  for (const [, m] of loaded) mods.push(pad(m.name, 26) + ' ' + pad(m.version || '', 9) + ' ' + (m.file || ''));
  writeText('modules.txt', mods.join('\n') + (mods.length ? '\n' : ''));
  writeText('history.txt', history.slice(-200).join('\n') + (history.length ? '\n' : ''));
}

// ---------------------------------------------------------------------------------------------------------------
// Modules: what the session loads, and their TypeScript declarations

function packageDirOf(file) {
  let dir = path.dirname(file);
  for (;;) {
    if (fs.existsSync(path.join(dir, 'package.json'))) return dir;
    const up = path.dirname(dir);
    if (up === dir || path.basename(dir) === 'node_modules') return null;
    dir = up;
  }
}

function readJson(file) {
  try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch (e) { return null; }
}

function noteLoad(request, file, exports) {
  if (!request || request.startsWith('.') && !file) return;
  const builtin = Module.isBuiltin(request);
  const key = builtin ? 'node:' + request.replace(/^node:/, '') : file || request;
  if (loaded.has(key)) { if (exports !== undefined) loaded.get(key).exports = exports; return; }
  const entry = {name: builtin ? key : request, file: builtin ? '(built in)' : file || '', version: '', exports, request};
  if (!builtin && file) {
    const dir = packageDirOf(file);
    const pkg = dir && readJson(path.join(dir, 'package.json'));
    if (pkg) {
      entry.version = pkg.version || '';
      if (request.startsWith('.') || path.isAbsolute(request)) entry.name = (pkg.name ? pkg.name + ':' : '') + path.relative(dir, file);
    }
  }
  loaded.set(key, entry);
}

const realLoad = Module._load;
Module._load = function (request, parent, isMain) {
  const exports = realLoad.apply(this, arguments);
  try {
    if (parent && (parent.id === '<repl>' || sessionFiles.has(parent.filename))) {
      let file = '';
      if (!Module.isBuiltin(request)) { try { file = Module._resolveFilename(request, parent); } catch (e) { /* loaded anyway */ } }
      noteLoad(request, file, exports);
    }
  } catch (e) { /* bookkeeping only */ }
  return exports;
};

function resolveFrom(spec, base) {
  const req = Module.createRequire(base || path.join(process.cwd(), '<repl>'));
  try { return req.resolve(spec); } catch (e) { /* a types-only package, or not installed */ }
  try { return req.resolve(spec + '/package.json'); } catch (e) { return ''; }
}

// The declaration file for a package root: `types` / `typings` / exports["."].types, then main's .d.ts, then index.d.ts.
function typesOfPackage(dir) {
  const pkg = readJson(path.join(dir, 'package.json')) || {};
  const cands = [];
  const ex = pkg.exports && (typeof pkg.exports === 'object') ? (pkg.exports['.'] || pkg.exports) : null;
  const exTypes = ex && typeof ex === 'object' ? (ex.types || (ex.require && ex.require.types) || (ex.import && ex.import.types)) : null;
  for (const t of [pkg.types, pkg.typings, typeof exTypes === 'string' ? exTypes : null]) if (t) cands.push(t);
  if (pkg.main) cands.push(pkg.main.replace(/\.(c|m)?js$|\.node$/, '') + '.d.ts', path.join(pkg.main, 'index.d.ts'));
  cands.push('index.d.ts');
  for (const c of cands) {
    const f = path.resolve(dir, c);
    if (fs.existsSync(f) && fs.statSync(f).isFile()) return f;
  }
  return '';
}

// The .d.ts that types `spec` (as required from `base`): beside the resolved file, the package's own, then @types.
function typesFor(spec, base) {
  if (Module.isBuiltin(spec)) return '';
  const file = resolveFrom(spec, base);
  if (file) {
    const sib = file.replace(/\.(c|m)?js$|\.node$/, (m, x) => '') + (/\.mjs$/.test(file) ? '.d.mts' : /\.cjs$/.test(file) ? '.d.cts' : '.d.ts');
    if (!/\.json$/.test(file) && sib !== file && fs.existsSync(sib)) return sib;
    const dir = packageDirOf(file);
    if (dir) { const t = typesOfPackage(dir); if (t) return t; }
  }
  const bare = /^(@[^/]+\/[^/]+|[^./][^/]*)/.exec(spec);
  if (bare) {
    const name = bare[1].startsWith('@') ? bare[1].slice(1).replace('/', '__') : bare[1];
    const t = resolveFrom('@types/' + name + '/package.json', base);
    if (t) return typesOfPackage(path.dirname(t));
  }
  return '';
}

// ---------------------------------------------------------------------------------------------------------------
// A small .d.ts reader, for when mep-js-lsp --dts-doc is not available

const dtsCache = new Map();

function jsdocMarkdown(doc) {
  const lines = doc.replace(/^\/\*\*\s?/, '').replace(/\s*\*\/$/, '').split('\n').map(l => l.replace(/^\s*\* ?/, ''));
  return lines.join('\n').trim()
    .replace(/\n?^@param\s+(?:\{[^}]*\}\s*)?(\[?[\w$.]+\]?)\s*-?\s*/gm, '\n\n*@param* `$1` — ')
    .replace(/\n?^@(returns?|throws|example|deprecated|see|since|default)\b\s*/gm, '\n\n*@$1* ')
    .replace(/\n{3,}/g, '\n\n').trim();
}

function parseDts(text) {
  const entries = new Map();  // qualified name -> {sigs: [], docs: [], kind}
  const exported = [];
  entries.exported = exported;
  const add = (name, sig, doc) => {
    if (!entries.has(name)) entries.set(name, {sigs: [], docs: []});
    const e = entries.get(name);
    e.sigs.push(sig.replace(/^export\s+/, '').replace(/^declare\s+/, '').replace(/\s*\{\s*$/, '').replace(/;\s*$/, ''));
    if (doc && !e.docs.includes(doc)) e.docs.push(doc);
  };
  const lines = text.split('\n');
  const stack = [{name: '', kind: 'module', depth: 0}];
  let depth = 0, doc = '', inDoc = false, docLines = [], inComment = false;
  const declRe = /^(?:export\s+)?(?:declare\s+)?(?:default\s+)?(?:abstract\s+)?(function|class|interface|const\s+enum|enum|namespace|module|type|const|let|var)\s+([\w$.]+|"[^"]+"|'[^']+')/;
  const memberRe = /^(?:(?:public|private|protected|static|readonly|abstract|declare|override|async|get|set)\s+)*(constructor|[\w$]+|"[^"]+")\??\s*[(<:;]/;
  const enumRe = /^([\w$]+|"[^"]+")\s*(?:=|,|$)/;
  const braces = s => {
    let d = 0;
    s = s.replace(/\/\/.*$/, '').replace(/"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|`(?:[^`\\]|\\.)*`/g, '""');
    for (const c of s) { if (c === '{') d++; else if (c === '}') d--; }
    return d;
  };
  for (let li = 0; li < lines.length; li++) {
    let line = lines[li].trim();
    if (inDoc) {
      docLines.push(line);
      if (line.includes('*/')) { inDoc = false; doc = jsdocMarkdown(docLines.join('\n')); }
      continue;
    }
    if (inComment) { if (line.includes('*/')) inComment = false; continue; }
    if (line.startsWith('/**')) {
      docLines = [line];
      if (line.includes('*/', 3)) doc = jsdocMarkdown(line); else inDoc = true;
      continue;
    }
    if (line.startsWith('/*')) { if (!line.includes('*/')) inComment = true; continue; }
    if (line === '' || line.startsWith('//')) continue;
    const top = stack[stack.length - 1];
    if (depth === top.depth) {
      // A declaration over several lines: until its brackets close and it ends in ';', '{' or '}'.
      let full = line;
      let parens = (s => (s.match(/[(<[]/g) || []).length - (s.match(/[)>\]]/g) || []).length + (s.match(/=>/g) || []).length)(line);
      while (parens > 0 && li + 1 < lines.length && full.length < 2000) {
        const next = lines[++li].trim();
        full += ' ' + next;
        parens += (next.match(/[(<[]/g) || []).length - (next.match(/[)>\]]/g) || []).length + (next.match(/=>/g) || []).length;
      }
      const prefix = top.name ? top.name + '.' : '';
      let m;
      if ((m = declRe.exec(full))) {
        const kind = m[1].replace(/\s+/, ' ');
        const name = m[2].replace(/^['"]|['"]$/g, '');
        add(prefix + name, full, doc);
        entries.get(prefix + name).kind = kind;
        if (!top.name) exported.push(name);
        const open = braces(full);
        const body = /enum/.test(kind) && open === 0 && /\{(.*)\}/.exec(full);
        if (body) {
          for (const part of body[1].split(',')) {
            const em = enumRe.exec(part.trim());
            if (em) add(prefix + name + '.' + em[1].replace(/^"|"$/g, ''), name + '.' + part.trim(), doc);
          }
        }
        if (open > 0 && /class|interface|enum|namespace|module/.test(kind)) {
          stack.push({name: /module/.test(kind) && /^["']/.test(m[2]) ? '' : prefix + name, kind, depth: depth + 1});
        }
        depth += open;
      } else if (top.kind === 'enum' && (m = enumRe.exec(full))) {
        add(prefix + m[1].replace(/^"|"$/g, ''), top.name.split('.').pop() + '.' + full.replace(/,\s*$/, ''), doc || (entries.get(top.name) || {docs: []}).docs[0]);
        depth += braces(full);
      } else if ((top.kind === 'class' || top.kind === 'interface') && (m = memberRe.exec(full))) {
        add(prefix + m[1].replace(/^"|"$/g, ''), top.name.split('.').pop() + '.' + full, doc);
        depth += braces(full);
      } else {
        depth += braces(full);
      }
    } else {
      depth += braces(line);
    }
    while (stack.length > 1 && depth < stack[stack.length - 1].depth) stack.pop();
    doc = '';
  }
  return entries;
}

function dtsEntries(file) {
  let mtime = 0;
  try { mtime = fs.statSync(file).mtimeMs; } catch (e) { return null; }
  const c = dtsCache.get(file);
  if (c && c.mtime === mtime) return c.entries;
  const entries = parseDts(fs.readFileSync(file, 'utf8'));
  dtsCache.set(file, {mtime, entries});
  return entries;
}

function dtsDocFallback(file, name) {
  const entries = dtsEntries(file);
  if (!entries) return null;
  const footer = '\n\n*declared in `' + file + '`*';
  if (name === '') return '**Exports:** ' + [...new Set(entries.exported)].sort().map(x => '`' + x + '`').join(', ') + footer;
  const e = entries.get(name);
  if (!e) return null;
  let sigs = e.sigs, docs = e.docs.slice(), extra = '';
  if (e.kind === 'class' || e.kind === 'interface') {
    const ctor = entries.get(name + '.constructor');
    if (ctor) { sigs = sigs.concat(ctor.sigs.map(x => 'new ' + x.replace(/^[\w$]+\.constructor/, name.split('.').pop()))); docs = docs.concat(ctor.docs); }
    const members = [...entries.keys()].filter(k => k.startsWith(name + '.') && !k.slice(name.length + 1).includes('.') && !k.endsWith('.constructor'));
    if (members.length) extra = '\n\n**Members:** ' + members.map(k => '`' + k.slice(name.length + 1) + '`').join(', ');
  }
  return '```ts\n' + sigs.join('\n') + '\n```' + (docs.length ? '\n\n' + docs.join('\n\n') : '') + extra + footer;
}

// The documentation `name` (`fn`, `Class.member`, `ns.fn`) has in the declaration file, or null.
function dtsDoc(file, name) {
  if (lspUsable) {
    const p = childProcess.spawnSync(MEP_JS_LSP, ['--dts-doc', file, name], {encoding: 'utf8', timeout: 20000});
    if (p.status === 0 && p.stdout.trim()) return p.stdout.trim();
    if (p.status === 1) return null;
    lspUsable = false;  // missing, or an older mep-js-lsp without --dts-doc
  }
  return dtsDocFallback(file, name);
}

// ---------------------------------------------------------------------------------------------------------------
// Help

// Names bound to modules in a source file: `const dm = require('m')`, `import * as dm from 'm'`,
// `import dm from 'm'`, `const {a, b: c} = require('m')`, `import {a as c} from 'm'`.
function sourceAliases(file) {
  const aliases = new Map();  // local name -> [specifier, exported name or '']
  let text = '';
  try { text = fs.readFileSync(file, 'utf8'); } catch (e) { return aliases; }
  let m;
  const whole = /(?:const|let|var)\s+([\w$]+)\s*=\s*(?:await\s+)?(?:require|import)\(\s*(['"])([^'"]+)\2\s*\)/g;
  while ((m = whole.exec(text))) aliases.set(m[1], [m[3], '']);
  const star = /import\s+(?:[\w$]+\s*,\s*)?\*\s*as\s+([\w$]+)\s+from\s*(['"])([^'"]+)\2/g;
  while ((m = star.exec(text))) aliases.set(m[1], [m[3], '']);
  const def = /import\s+([\w$]+)\s*(?:,\s*[{*][^;]*?)?\s+from\s*(['"])([^'"]+)\2/g;
  while ((m = def.exec(text))) if (m[1] !== 'type') aliases.set(m[1], [m[3], 'default']);
  const destr = /(?:(?:const|let|var)\s*\{([^}]*)\}\s*=\s*(?:await\s+)?(?:require|import)\(|import\s+(?:[\w$]+\s*,\s*)?\{([^}]*)\}\s*from)\s*(['"])([^'"]+)\3/g;
  while ((m = destr.exec(text))) {
    for (const part of (m[1] || m[2]).split(',')) {
      const p = part.trim().replace(/\s*=.*$/, '');
      const r = /^([\w$]+)\s*(?::|\bas\b)\s*([\w$]+)$/.exec(p);
      if (r) aliases.set(r[2], [m[4], r[1]]);
      else if (/^[\w$]+$/.test(p)) aliases.set(p, [m[4], p]);
    }
  }
  return aliases;
}

// The session's value of a dotted name, or {found: false}.
function lookup(parts) {
  let v;
  if (Object.prototype.hasOwnProperty.call(globalThis, parts[0])) v = globalThis[parts[0]];
  else {
    try { v = require('vm').runInThisContext(parts[0]); } catch (e) { return {found: false}; }
  }
  for (let k = 1; k < parts.length; k++) {
    if (v === null || v === undefined) return {found: false};
    const parent = v;
    try { v = parent[parts[k]]; } catch (e) { return {found: false}; }
    if (v === undefined && !(parts[k] in Object(parent))) return {found: false};
  }
  return {found: true, value: v};
}

// Where a session value's documentation lives: [{dts, name}] candidates, most specific first.
function docTargets(parts, fromFile) {
  const out = [];
  const seen = new Set();
  const push = (dts, name) => { if (dts && name && !seen.has(dts + '\0' + name)) { seen.add(dts + '\0' + name); out.push({dts, name}); } };
  const typesOfEntry = m => {
    if (m.dts === undefined) m.dts = m.name.startsWith('node:') ? '' : typesFor(m.file && fs.existsSync(m.file) ? m.file : m.request, m.base);
    return m.dts;
  };
  const root = lookup(parts.slice(0, 1));
  const rest = parts.slice(1).join('.');
  if (root.found && root.value !== null && (typeof root.value === 'object' || typeof root.value === 'function')) {
    for (const [, m] of loaded) {
      const dts = typesOfEntry(m);
      if (!dts) continue;
      const isNs = m.exports && Object.prototype.toString.call(m.exports) === '[object Module]';
      for (const ex of isNs ? [m.exports, m.exports.default] : [m.exports]) {
      if (ex === undefined || ex === null || (typeof ex !== 'object' && typeof ex !== 'function')) continue;
      if (ex === root.value) {
        if (rest) push(dts, rest);
        else out.push({dts, name: '', module: m});
        continue;
      }
      for (const key of Object.keys(ex)) {
        let val;
        try { val = ex[key]; } catch (e) { continue; }
        if (val === root.value) push(dts, [key, ...parts.slice(1)].join('.'));
        else if (typeof val === 'function' && typeof root.value === 'object' && root.value instanceof val &&
                 Object.getPrototypeOf(root.value) === val.prototype && parts.length > 1) push(dts, [key, ...parts.slice(1)].join('.'));
      }
      }
    }
  }
  if (fromFile) {
    const alias = sourceAliases(fromFile).get(parts[0]);
    if (alias) {
      const dts = typesFor(alias[0], fromFile);
      const name = [alias[1] === 'default' ? '' : alias[1], ...parts.slice(1)].filter(Boolean).join('.');
      if (dts && name) push(dts, name);
      else if (dts) out.push({dts, name: '', module: {name: alias[0]}});
    }
  }
  // Not a session value: any loaded module's export of that name.
  for (const [, m] of loaded) push(typesOfEntry(m), parts.join('.'));
  if (fromFile) for (const [, [spec]] of sourceAliases(fromFile)) push(typesFor(spec, fromFile), parts.join('.'));
  return out;
}

function runtimeHelp(name, v) {
  const lines = [];
  if (typeof v === 'function') {
    lines.push(signature(v, name.split('.').pop()));
    const src = Function.prototype.toString.call(v);
    const statics = Object.getOwnPropertyNames(v).filter(k => !['length', 'name', 'prototype', 'caller', 'arguments'].includes(k));
    const proto = v.prototype ? Object.getOwnPropertyNames(v.prototype).filter(k => k !== 'constructor') : [];
    if (statics.length) lines.push('', 'static: ' + statics.slice(0, 80).join(', '));
    if (proto.length) lines.push('', 'methods: ' + proto.slice(0, 120).join(', '));
    if (!/\[native code\]/.test(src) && !/^class\b/.test(src)) {
      const body = src.split('\n');
      lines.push('', '```js', ...body.slice(0, 40), ...(body.length > 40 ? ['...'] : []), '```');
    }
  } else if (v !== null && typeof v === 'object') {
    lines.push(name + ': ' + kindOf(v));
    const keys = Object.keys(v);
    const rows = [];
    for (const k of keys.slice(0, 200)) {
      let x;
      try { x = v[k]; } catch (e) { continue; }
      rows.push('  ' + pad(k, 24) + ' ' + pad(kindOf(x), 10) + ' ' + (typeof x === 'function' ? signature(x, k) : detail(x)));
    }
    if (rows.length) lines.push('', ...rows, ...(keys.length > 200 ? ['  ...'] : []));
  } else {
    lines.push(name + ' = ' + util.inspect(v) + '  (' + kindOf(v) + ')');
  }
  return lines.join('\n');
}

function helpText(text, fromFile) {
  const name = text.trim().replace(/\?\./g, '.').replace(/^\.+|\.+$/g, '');
  if (!name) return 'usage: :help name   (name, mod.name, Class.method, a module specifier)\n';
  const parts = name.split('.');
  // A module specifier (`@scope/pkg`, `node:fs`, `./lib`) rather than a dotted name.
  const isSpec = !parts.every(p => /^[\w$]+$/.test(p));
  const builtinName = Module.isBuiltin(name) ? name.replace(/^node:/, '') : null;
  const found = isSpec ? {found: false} : lookup(parts);
  const sections = [];
  if (!isSpec) {
    for (const t of docTargets(parts, fromFile)) {
      if (t.name === '') {
        sections.push('# ' + name + ' (module ' + (t.module ? t.module.name : '') + ')\n\n' + (dtsDoc(t.dts, '') || ''));
        break;
      }
      const doc = dtsDoc(t.dts, t.name);
      if (doc) { sections.push(doc); break; }
    }
  }
  if (!builtinName && !found.found && !sections.length) {
    const dts = typesFor(name, fromFile);
    if (dts) sections.push('# module ' + name + '\n\n' + (dtsDoc(dts, '') || ''));
    try {
      const file = resolveFrom(name, fromFile);
      if (file && !/\.json$/.test(file)) sections.push(runtimeHelp(name, Module.createRequire(fromFile || path.join(process.cwd(), '<repl>'))(name)));
    } catch (e) { /* not loadable here */ }
  }
  if (builtinName && !found.found) {
    try { sections.push(runtimeHelp('node:' + builtinName, require(builtinName))); } catch (e) { /* not loadable */ }
  }
  if (found.found) sections.push(runtimeHelp(name, found.value));
  const rootFound = found.found && (parts.length === 1 ? found : lookup(parts.slice(0, 1)));
  const mod = builtinName || (rootFound && rootFound.found && loadedBuiltin(rootFound.value, parts[0]));
  if (mod) sections.push('Node.js documentation: https://nodejs.org/api/' + mod.replace(/\/.*/, '') + '.html');
  if (!sections.length) return name + ': not a name in this session, nor in the TypeScript declarations of a loaded module.\n';
  return sections.join('\n\n---\n\n') + '\n';
}

// The built-in module `v` is, when the session's `name` is bound to one (the REPL's `fs`, a `require('fs')`).
// Only modules already loaded are compared: requiring every built-in would load deprecated ones.
function loadedBuiltin(v, name) {
  if (v === null || typeof v !== 'object') return null;
  if (Module.isBuiltin(name)) { try { if (require(name) === v) return name.replace(/^node:/, ''); } catch (e) { /* skip */ } }
  for (const [key, m] of loaded) if (key.startsWith('node:') && m.exports === v) return key.slice(5);
  return null;
}

function help(text, fromFile) {
  let out;
  try { out = helpText(text, fromFile); } catch (e) { out = text + ': ' + (e && e.stack || e) + '\n'; }
  writeText('help.txt', out);
  console.log('// ' + text.trim() + ': in the Help tab');
}

function inspect(text) {
  const name = text.trim();
  const found = /^[\w$]+(\.[\w$]+)*$/.test(name) ? lookup(name.split('.')) : {found: false};
  let out;
  if (!found.found) out = name + ': not a name in this session\n';
  else out = name + '  (' + kindOf(found.value) + ')\n\n' + util.inspect(found.value, {depth: 4, maxArrayLength: 200, breakLength: 100, showHidden: false}) + '\n';
  writeText('inspect.txt', out);
}

// ---------------------------------------------------------------------------------------------------------------
// The REPL

const server = repl.start({prompt: '> ', useGlobal: true});
const stockEval = server.eval;
baseGlobals = new Set(Object.getOwnPropertyNames(globalThis));
if (process.env.NODE_REPL_HISTORY !== '') {
  const file = process.env.NODE_REPL_HISTORY || path.join(require('os').homedir(), '.node_repl_history');
  try { server.setupHistory(file, () => {}); } catch (e) { /* no history file */ }
}

// A sent chunk's error thrown at top level goes to the REPL's domain, not to the callback.
server._domain.prependListener('error', e => {
  if (sourceEval) sourceStack(e);
  sourceEval = false;
  try { refresh(); } catch (x) { /* panels are best effort */ }
});

function quiet(cb) {
  return (err, value) => {
    try { refresh(); } catch (e) { /* panels are best effort */ }
    const saved = server.ignoreUndefined;
    server.ignoreUndefined = true;
    try { cb(err, value); } finally { server.ignoreUndefined = saved; }
  };
}

function stringArgs(s) {
  return (s.match(/"(?:[^"\\]|\\.)*"/g) || []).map(a => JSON.parse(a));
}

// Evaluates `file` (the source's code, padded to its line numbers) under the source's name.
function evalFile(file, name, cb) {
  let code;
  try { code = fs.readFileSync(file, 'utf8'); } catch (e) { return cb(e); }
  name = name || file;
  if (path.isAbsolute(name)) {
    sessionFiles.add(name);
    globalThis.require = Module.createRequire(name);
    globalThis.__filename = name;
    globalThis.__dirname = path.dirname(name);
  }
  const first = code.split('\n').findIndex(l => l.trim() !== '');
  const count = code.replace(/\n+$/, '').split('\n').length;
  history.push('// ' + path.basename(name) + ':' + (first + 1) + (count > first + 1 ? '-' + count : '') + '  ' +
    oneLine(code.split('\n')[first] || '', 70));
  sourceEval = true;
  stockEval.call(server, transform(code), server.context, name, (err, value) => {
    sourceEval = false;
    // An incomplete chunk is a syntax error here, not a cue to wait for more input.
    if (err instanceof repl.Recoverable) err = err.err;
    cb(sourceStack(err), value);
  });
}

// The REPL shows no frame for top-level code; a sent chunk's error keeps its frames down to the evaluator's
// own, so the source file's line is in the message.
let sourceEval = false;
function sourceStack(e) {
  if (!e || typeof e !== 'object' || e.name === 'SyntaxError') return e;
  let stack;
  try { stack = String(e.stack); } catch (x) { return e; }
  const lines = stack.split('\n');
  const cut = lines.findIndex((l, k) => k > 0 && /^\s+at .*(node:vm|node:repl|node:domain|ContextifyScript)/.test(l));
  if (cut > 0) {
    try { Object.defineProperty(e, 'stack', {value: lines.slice(0, cut).join('\n'), configurable: true, writable: true}); } catch (x) { /* frozen */ }
  }
  return e;
}

server.eval = function mepEval(cmd, context, file, cb) {
  const m = /^\s*:(help|inspect|eval|run|refresh)\b\s*([\s\S]*?)\s*$/.exec(cmd);
  if (m) {
    const [, verb, arg] = m;
    if (verb === 'help') {
      const args = stringArgs(arg);
      const name = arg.startsWith('"') ? args[0] : arg.split(/\s+/)[0];
      help(name || '', arg.startsWith('"') ? args[1] : args[0]);
      history.push(':help ' + name);
    } else if (verb === 'inspect') {
      inspect(arg);
    } else if (verb === 'eval' || verb === 'run') {
      const [f, name] = stringArgs(arg).concat(arg.startsWith('"') ? [] : [arg]);
      return evalFile(f, name, quiet(cb));
    }
    return quiet(cb)(null, undefined);
  }
  if (cmd.trim()) history.push(oneLine(cmd.trim(), 100));
  stockEval.call(this, transform(cmd), context, file, (err, value) => {
    if (err instanceof repl.Recoverable) history.pop();
    try { refresh(); } catch (e) { /* panels are best effort */ }
    cb(err, value);
  });
};

// What a rewritten static `import` calls: the specifier resolved as require() would (NODE_PATH, relative to the
// file being evaluated), then loaded with import().
async function importModule(spec) {
  const base = typeof globalThis.__filename === 'string' && path.isAbsolute(globalThis.__filename)
    ? globalThis.__filename : path.join(process.cwd(), '<repl>');
  let target = spec, file = '';
  if (!Module.isBuiltin(spec)) {
    try { file = Module.createRequire(base).resolve(spec); target = require('url').pathToFileURL(file).href; } catch (e) {
      if (spec.startsWith('.')) { file = path.resolve(path.dirname(base), spec); target = require('url').pathToFileURL(file).href; }
    }
  }
  const ns = await import(target);
  noteLoad(spec, file, ns);
  return ns;
}

// Named imports from a CommonJS module whose exports Node cannot list statically (a native addon): its
// module.exports' own properties, then the namespace's.
function named(ns) {
  const d = ns && ns.default;
  if (d === null || (typeof d !== 'object' && typeof d !== 'function')) return ns;
  const out = {};
  for (const k of Object.keys(d)) { try { out[k] = d[k]; } catch (e) { /* a throwing getter */ } }
  for (const k of Object.keys(ns)) out[k] = ns[k];
  return out;
}

globalThis.__mep = {help, inspect, refresh, transform, typesFor, dtsDoc, helpText, evalFile, import: importModule, named};
refresh();
console.log('Node.js ' + process.version + ' -- mep language UI: <leader>re runs the statement at the cursor, <leader>rr the file; ' +
  ':help name / gh shows help, :inspect name inspects.');
server.displayPrompt();
]==]

-- tab id -> session state. The sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state = {}
local sidebars = nil

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    variables = mep.sidebar_create('Variables', 'right', 44),
    modules = mep.sidebar_create('Modules', 'right', 44),
    history = mep.sidebar_create('History', 'right', 44),
    inspect = mep.sidebar_create('Inspect', 'right', 44),
    help = mep.sidebar_create('Help', 'right', 44),
  }
end

-- A JavaScript (and JSON) string literal.
local function jsquote(s)
  s = tostring(s):gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
  return '"' .. s .. '"'
end
mep.javascript_ui_quote = jsquote

local function send(st, text)
  mep.terminal_write(st.console_buf, text .. '\n')
end

local function render_list(id, text, empty, click)
  local widgets = {}
  for line in (text or ''):gmatch('[^\n]+') do
    local name = line:match('^(%S+)')
    widgets[#widgets + 1] = {id = line, text = line, tooltip = (click and name) and (click.tip .. ' ' .. name) or nil,
      on_click = (click and name) and function() click.fn(name) end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = empty} end
  mep.sidebar_set_sections(id, {{id = 'list', title = '', collapsed = false, widgets = widgets}})
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  render_list(sidebars.variables, st and st.variables_text, '(nothing defined in this session yet)',
    st and {tip = 'inspect', fn = function(name) send(st, ':inspect ' .. name) end})
  render_list(sidebars.modules, st and st.modules_text, "(nothing loaded yet -- require('pkg') or import in the console)",
    st and {tip = 'help', fn = function(name)
      send(st, ':help ' .. jsquote(name))
      if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
    end})
  render_list(sidebars.history, st and st.history_text, '(nothing evaluated yet)', nil)
  mep.language_ui_render_textbox(sidebars.inspect, st and st.inspect_text,
    '(nothing inspected yet -- click a Variables row, or :inspect name in the console)')
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    '(no help viewed yet -- gh over a name, or :help name in the console)')
end

-- Per line: the bracket depth at its start and whether it starts inside a
-- block comment or template literal. Strings, comments and templates are
-- skipped; a `/` after an operator starts a regular expression.
local function scan_lines(lines)
  local depth, inside = {}, {}
  local d, mode = 0, nil   -- mode: nil, 'comment', 'template'
  local tdepth = {}        -- bracket depth at each open `${`
  for i, l in ipairs(lines) do
    depth[i], inside[i] = d, mode ~= nil
    local p, n, prev = 1, #l, ''
    while p <= n do
      local c = l:sub(p, p)
      if mode == 'comment' then
        local e = l:find('*/', p, true)
        if not e then p = n + 1 else mode = nil p = e + 2 end
      elseif mode == 'template' then
        if c == '\\' then p = p + 2
        elseif c == '`' then mode = nil p = p + 1 prev = '`'
        elseif c == '$' and l:sub(p + 1, p + 1) == '{' then tdepth[#tdepth + 1] = d d = d + 1 mode = nil p = p + 2
        else p = p + 1 end
      elseif c == '/' and l:sub(p + 1, p + 1) == '/' then p = n + 1
      elseif c == '/' and l:sub(p + 1, p + 1) == '*' then mode = 'comment' p = p + 2
      elseif c == '"' or c == "'" then
        p = p + 1
        while p <= n and l:sub(p, p) ~= c do if l:sub(p, p) == '\\' then p = p + 1 end p = p + 1 end
        p = p + 1 prev = c
      elseif c == '`' then mode = 'template' p = p + 1
      elseif c == '/' and (prev == '' or prev:match('[%(,=:%[!&|?{};+%-*%%<>~^]')) then
        p = p + 1
        local class = false
        while p <= n do
          local x = l:sub(p, p)
          if x == '\\' then p = p + 1
          elseif x == '[' then class = true
          elseif x == ']' then class = false
          elseif x == '/' and not class then break end
          p = p + 1
        end
        p = p + 1 prev = '/'
      else
        if c == '(' or c == '[' or c == '{' then d = d + 1
        elseif c == ')' or c == ']' or c == '}' then
          if c == '}' and #tdepth > 0 and tdepth[#tdepth] == d - 1 then
            table.remove(tdepth) d = d - 1 mode = 'template'
          else d = math.max(0, d - 1) end
        end
        if not c:match('%s') then prev = c end
        p = p + 1
      end
    end
  end
  return depth, inside
end

-- The top-level statement around `row`: from the nearest line at or above it
-- that starts a statement at column 0 (outside every bracket, not a closing
-- bracket, not a comment, not continuing the line before) to just before the
-- next. A comment directly above a statement goes with it when the cursor is
-- on the comment. Returns the first and last rows, or nil.
function mep.javascript_ui_block_at(lines, row)
  local n = #lines
  local depth, inside = scan_lines(lines)
  local function code(k)
    return (lines[k] or ''):gsub('%s+//.*$', ''):gsub('%s+$', '')
  end
  local function comment(k)
    local l = lines[k] or ''
    return l:match('^%s*//') or l:match('^%s*/%*') or (inside[k] and l:match('^%s*%*'))
  end
  local function continues(k)
    local j = k - 1
    while j >= 1 and (lines[j]:match('^%s*$') or comment(j)) do j = j - 1 end
    if j < 1 then return false end
    local p = code(j)
    if p:match('[,=+%-*/%%&|^!<>?:.%(%[{]$') and not p:match('%+%+$') and not p:match('%-%-$') and not p:match('%*/$') then
      return true
    end
    local l = lines[k]
    if l:match('^[.?:]') and not l:match('^%.%.%.') then return true end
    if l:match('^[+*/%%&|^<>=]') or l:match('^%-[^%-]') then return true end
    return false
  end
  local function starts(k)
    local l = lines[k]
    return l ~= nil and depth[k] == 0 and not inside[k] and l:match('^[^%s]') ~= nil and not comment(k)
      and not l:match('^[%)%]}]') and not continues(k)
  end
  local s = row
  if comment(s) or (lines[s] or ''):match('^%s*$') then
    -- On a comment: the statement it documents, when it comes right after.
    local k = s
    while k <= n and comment(k) do k = k + 1 end
    if k <= n and starts(k) and comment(s) then
      local e = k
      while e < n and not starts(e + 1) do e = e + 1 end
      while e > k and (lines[e]:match('^%s*$') or comment(e)) do e = e - 1 end
      return k, e
    end
  end
  while s > 1 and not starts(s) do s = s - 1 end
  if not starts(s) then return nil end
  local e = s
  while e < n and not starts(e + 1) do e = e + 1 end
  while e > s and (lines[e]:match('^%s*$') or comment(e)) do e = e - 1 end
  return s, e
end

-- Writes rows first..last of `lines` to the session's eval file, padded so
-- its line numbers are the source's, and has the console run it.
local function eval_text(st, lines, first, last)
  local chunk = {}
  for i = first, last do chunk[#chunk + 1] = lines[i] end
  local text = string.rep('\n', first - 1) .. table.concat(chunk, '\n') .. '\n'
  mep.language_ui_write_file(st.eval_path, text)
  local fname = mep.filename and mep.filename()
  local name = (fname and fname ~= '') and mep_lsp_abspath(fname) or 'REPL'
  send(st, ':eval ' .. jsquote(st.eval_path) .. ' ' .. jsquote(name))
end

-- <leader>re: the Visual selection if one is live, else the top-level
-- statement at the cursor.
function mep.javascript_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Node.js console in this tab -- <leader>uu opens one', 'warn')
    return
  end
  local lines = {}
  for i = 1, mep.line_count() do lines[i] = mep.get_line(i) or '' end
  local sel = mep.visual_selection and mep.visual_selection() or ''
  if sel ~= '' then
    local r = mep.visual_range()
    mep.cmd('normal!')
    if r and r.kind == 'char' then
      local sel_lines = {}
      for l in (sel .. '\n'):gmatch('([^\n]*)\n') do sel_lines[#sel_lines + 1] = l end
      local padded = {}
      for i = 1, r.start_row - 1 do padded[i] = '' end
      for i, l in ipairs(sel_lines) do padded[r.start_row - 1 + i] = l end
      eval_text(st, padded, r.start_row, r.start_row + #sel_lines - 1)
    elseif r then
      eval_text(st, lines, r.start_row, r.end_row)
    end
    return
  end
  local s, e = mep.javascript_ui_block_at(lines, (mep.cursor()))
  if not s then mep.notify('No JavaScript statement at the cursor', 'warn') return end
  eval_text(st, lines, s, e)
  if s ~= e then mep.notify(string.format('Sent lines %d-%d to the console', s, e)) end
end
mep.javascript_ui_eval_selection = mep.javascript_ui_eval_block
mep.language_ui_eval_handlers.javascript = mep.javascript_ui_eval_block

function mep.javascript_ui_open()
  local session_dir = mep.language_ui_session_dir('jsui')
  local paths = {
    variables = session_dir .. '/variables.txt', modules = session_dir .. '/modules.txt',
    history = session_dir .. '/history.txt', inspect = session_dir .. '/inspect.txt', help = session_dir .. '/help.txt',
  }
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local init_path = session_dir .. '/mep_init.js'
  local lsp = mep.bundled_tool and mep.bundled_tool('mep-js-lsp') or 'mep-js-lsp'
  mep.language_ui_write_file(init_path, 'const MEP_DIR = ' .. jsquote(session_dir) .. ';\nconst MEP_JS_LSP = '
    .. jsquote(lsp) .. ';\n' .. mep_javascript_ui_init_template)
  local src_buf = mep.current_buffer()
  local argv = mep.language_ui_cmd(mep.opt.javascript_ui_cmd)
  argv[#argv + 1] = init_path
  local layout = mep.language_ui_layout({
    info_share = mep.opt.javascript_ui_info_share, bottom_share = mep.opt.javascript_ui_bottom_share,
    bottom_info_share = mep.opt.javascript_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'node') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = session_dir, console_buf = layout.console_buf, editor_pane = layout.editor_pane, src_buf = src_buf,
    eval_path = session_dir .. '/eval.js', run_path = session_dir .. '/run.js',
    variables_text = '', modules_text = '', history_text = '', inspect_text = '', help_text = '', help_buf = nil,
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.variables, sidebars.modules, sidebars.history, sidebars.inspect})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('JavaScript language UI: node below; Variables/Modules/History/Inspect top-right, Help bottom-right; <leader>re runs the statement at the cursor, <leader>rr the file')
  -- run_source: <leader>rr / the Run button while the mode is open -- the
  -- source buffer as it is now (unsaved edits too), evaluated in the
  -- session under the file's own name.
  local function run_source(fname)
    local abs = mep_lsp_abspath(fname)
    local lines = mep.buffer_get_lines(st.src_buf)
    if not lines then
      send(st, ':run ' .. jsquote(abs) .. ' ' .. jsquote(abs))
      return
    end
    mep.language_ui_write_file(st.run_path, table.concat(lines, '\n') .. '\n')
    send(st, ':run ' .. jsquote(st.run_path) .. ' ' .. jsquote(abs))
  end
  return {
    run_source = run_source,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('JavaScript language UI mode closed')
    end,
  }
end
mep.language_ui_modes.javascript = {open = mep.javascript_ui_open}

-- gh: the session's documentation for the name under the cursor in the Help
-- tab -- the JSDoc of the package's TypeScript declarations, then the
-- runtime's view -- and from a JavaScript buffer the language server's hover
-- as well. The source file goes along, so a name it binds with require() or
-- import is found before the code has run.
function mep.javascript_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_$.]')
  if sym then sym = sym:gsub('^[.]+', ''):gsub('[.]+$', '') end
  if not sym or sym == '' then mep.notify('gh: no JavaScript name under the cursor', 'warn') return true end
  local from = ''
  if mep.language_for_buffer() == 'javascript' then
    local fname = mep.filename and mep.filename()
    if fname and fname ~= '' then from = ' ' .. jsquote(mep_lsp_abspath(fname)) end
  end
  send(st, ':help ' .. jsquote(sym) .. from)
  if mep.language_for_buffer() == 'javascript' then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
    if mep.lsp_hover then mep.lsp_hover() end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('javascript', mep.javascript_ui_help_at_cursor)

-- This tab's session as the panels see it (nil when no JavaScript mode is open).
function mep.javascript_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, variables = st.variables_text, modules = st.modules_text,
    history = st.history_text, inspect = st.inspect_text, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while a JavaScript
-- mode is open on the active tab (`st` defaults to that tab's session).
function mep.javascript_ui_poll(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
  local changed = false
  local function take(key, field)
    local text = mep.language_ui_read_file(st.paths[key])
    if text and text ~= st[field] then
      st[field] = text
      changed = true
      return true
    end
    return false
  end
  take('variables', 'variables_text')
  take('modules', 'modules_text')
  take('history', 'history_text')
  take('inspect', 'inspect_text')
  local help_changed = take('help', 'help_text')
  if changed then render_all() end
  if help_changed and st.help_buf then mep.reveal_buffer(st.help_buf, true) end
end
do
  local last_poll = 0
  mep.on_frame(function()
    local st = state[mep.current_tab_id()]
    if not st then return end
    local now = mep.now()
    if now - last_poll < mep.opt.language_ui_poll_interval then return end
    last_poll = now
    mep.javascript_ui_poll(st)
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

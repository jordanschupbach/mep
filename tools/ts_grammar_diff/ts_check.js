// The TypeScript oracle for tools/ts_grammar_diff (grammar_diff.py's
// 'typescript' and 'tsx' entries): does TypeScript's own parser accept a
// file? Exit 0 if `ts.createSourceFile` reports no parse diagnostics, 1
// (printing them) if it does. The script kind comes from the extension,
// as in tsc: .tsx files parse with JSX, .ts/.mts/.cts with type
// assertions, .d.ts as declaration files.
//
// Usage: node ts_check.js FILE...   (typescript from NODE_PATH, or from
// the `tsc` on PATH, e.g. `nix shell nixpkgs#nodejs nixpkgs#typescript`).

'use strict';

const fs = require('fs');
const path = require('path');

function loadTypescript() {
  try {
    return require('typescript');
  } catch (_) {
    for (const dir of (process.env.PATH || '').split(path.delimiter)) {
      const tsc = path.join(dir, 'tsc');
      if (!fs.existsSync(tsc)) continue;
      const lib = path.join(path.dirname(fs.realpathSync(tsc)), '..', 'lib', 'node_modules', 'typescript');
      if (fs.existsSync(lib)) return require(lib);
    }
    throw new Error('ts_check.js: no typescript module (NODE_PATH, or a tsc on PATH)');
  }
}

const ts = loadTypescript();
let failed = false;
for (const file of process.argv.slice(2)) {
  const text = fs.readFileSync(file, 'utf8');
  const kind = /\.tsx$/i.test(file) ? ts.ScriptKind.TSX : ts.ScriptKind.TS;
  const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, false, kind);
  for (const d of source.parseDiagnostics) {
    const { line, character } = source.getLineAndCharacterOfPosition(d.start);
    console.log(`${file}:${line + 1}:${character + 1}: ${ts.flattenDiagnosticMessageText(d.messageText, '\n')}`);
    failed = true;
  }
}
process.exit(failed ? 1 : 0);

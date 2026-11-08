// TypeScript oracle for tools/js_parse_ts_check.py (mep's TypeScript
// parser, src/js_parse_ts*.cpp):
//
//   node tools/js_parse_ts_oracle.js --errors LIST
//       LIST lines are "LANG PATH" (LANG ts, tsx, dts, mts or cts); prints one line
//       per file: "ok", or "error UPOS LINE:COL MESSAGE" for the first of
//       ts.createSourceFile's parseDiagnostics (what `tsc` reports as
//       syntax errors; UPOS is the UTF-16 offset)
//   node tools/js_parse_ts_oracle.js --shape-list LIST OUTDIR
//       for each line, OUTDIR/<line number>.txt: every node of
//       typescript-estree's tree as "TYPE START END" (UTF-16 offsets), or
//       "error MESSAGE" when typescript-estree throws
//
// typescript and @typescript-eslint/typescript-estree (+ visitor-keys) come
// from NODE_PATH.
"use strict";
const fs = require("fs");

const ts = require("typescript");

function kindOf(lang) {
  return lang === "tsx" ? ts.ScriptKind.TSX : ts.ScriptKind.TS;
}

function fileNameOf(lang) {
  return { tsx: "x.tsx", dts: "x.d.ts", mts: "x.mts", cts: "x.cts" }[lang] || "x.ts";
}

function readLines(list) {
  return fs
    .readFileSync(list, "utf8")
    .split("\n")
    .filter((l) => l.length)
    .map((l) => {
      const i = l.indexOf(" ");
      return [l.slice(0, i), l.slice(i + 1)];
    });
}

function errors(list) {
  const out = [];
  for (const [lang, file] of readLines(list)) {
    let text;
    try {
      text = fs.readFileSync(file, "utf8");
    } catch (_) {
      out.push("io_error");
      continue;
    }
    const sf = ts.createSourceFile(fileNameOf(lang), text, ts.ScriptTarget.Latest, false, kindOf(lang));
    const d = sf.parseDiagnostics[0];
    if (!d) {
      out.push("ok");
      continue;
    }
    const { line, character } = sf.getLineAndCharacterOfPosition(d.start);
    const msg = ts.flattenDiagnosticMessageText(d.messageText, " ").replace(/\n/g, " ");
    out.push(`error ${d.start} ${line + 1}:${character} ${msg}`);
  }
  process.stdout.write(out.join("\n") + "\n");
}

function shapes(list, outdir) {
  const estree = require("@typescript-eslint/typescript-estree");
  const { visitorKeys } = require("@typescript-eslint/visitor-keys");
  let n = 0;
  for (const [lang, file] of readLines(list)) {
    ++n;
    const lines = [];
    try {
      const text = fs.readFileSync(file, "utf8");
      const ast = estree.parse(text, {
        range: true,
        loc: false,
        jsx: lang === "tsx",
        filePath: fileNameOf(lang),
        allowInvalidAST: true,
        suppressDeprecatedPropertyWarnings: true,
      });
      const walk = (node) => {
        lines.push(`${node.type} ${node.range[0]} ${node.range[1]}`);
        // visitor-keys has no keys for the TSJSDoc* types; their child is
        // typeAnnotation
        const keys = visitorKeys[node.type] || (node.type.startsWith("TSJSDoc") ? ["typeAnnotation"] : []);
        for (const key of keys) {
          const v = node[key];
          const kids = Array.isArray(v) ? v : [v];
          for (const k of kids) if (k && typeof k.type === "string") walk(k);
        }
      };
      walk(ast);
    } catch (e) {
      lines.push("error " + String(e.message).split("\n")[0]);
    }
    fs.writeFileSync(`${outdir}/${n}.txt`, lines.join("\n") + "\n");
  }
}

const args = process.argv.slice(2);
if (args[0] === "--errors") errors(args[1]);
else if (args[0] === "--shape-list") shapes(args[1], args[2]);
else {
  process.stderr.write("usage: js_parse_ts_oracle.js --errors LIST | --shape-list LIST OUTDIR\n");
  process.exit(2);
}

// acorn oracle for tools/js_parse_check.py: prints, for each file, the JSON
// mep-js-parse-test --json prints, but produced by acorn 8 (+ acorn-jsx).
//
//   node tools/js_parse_oracle.js NODE_MODULES_DIR --list LIST OUTDIR
//       LIST lines are "MODE JSX PATH" (MODE module|script|auto, JSX 0/1);
//       writes OUTDIR/<line number>.json
//   node tools/js_parse_oracle.js NODE_MODULES_DIR [--module|--script|--auto] [--jsx] FILE
//
// NODE_MODULES_DIR holds acorn/ and acorn-jsx/ (passed explicitly: the
// eslint tree they usually come from has a symlink loop).
//
// Normalisation (JSON.stringify can't write these): a Literal's RegExp or
// BigInt `value` becomes null (its `regex` / `bigint` field carries it),
// a non-finite number becomes null.
"use strict";
const fs = require("fs");
const path = require("path");

const nm = process.argv[2];
const acorn = require(path.join(nm, "acorn"));
const jsx = require(path.join(nm, "acorn-jsx"));
const JsxParser = acorn.Parser.extend(jsx());

function replacer(key, value) {
  if (typeof value === "bigint") return null;
  if (value instanceof RegExp) return null;
  return value;
}

function parseOnce(src, sourceType, useJsx) {
  const comments = [];
  const P = useJsx ? JsxParser : acorn.Parser;
  const ast = P.parse(src, {
    ecmaVersion: "latest",
    sourceType,
    locations: true,
    onComment: comments,
  });
  return { ast, comments };
}

function run(src, mode, useJsx) {
  try {
    if (mode === "auto") {
      try {
        return parseOnce(src, "module", useJsx);
      } catch (e) {
        if (!(e instanceof SyntaxError)) throw e;
        try {
          return parseOnce(src, "script", useJsx);
        } catch (e2) {
          throw e;
        }
      }
    }
    return parseOnce(src, mode, useJsx);
  } catch (e) {
    if (!(e instanceof SyntaxError)) {
      return { crash: String(e && e.stack || e) };
    }
    return { error: e.message, pos: e.pos, line: e.loc.line, column: e.loc.column };
  }
}

function toJson(src, mode, useJsx) {
  const r = run(src, mode, useJsx);
  if (r.ast) {
    for (const c of r.comments) delete c.range;
    return JSON.stringify({ ast: r.ast, comments: r.comments }, replacer);
  }
  return JSON.stringify(r);
}

const args = process.argv.slice(3);
if (args[0] === "--list") {
  const lines = fs.readFileSync(args[1], "utf8").split("\n");
  const outDir = args[2];
  let n = 0;
  for (const line of lines) {
    if (!line) continue;
    ++n;
    const m = /^(\S+) (\S+) (.*)$/.exec(line);
    let out;
    try {
      const src = fs.readFileSync(m[3], "utf8");
      out = toJson(src, m[1], m[2] === "1");
    } catch (e) {
      out = JSON.stringify({ io_error: true });
    }
    fs.writeFileSync(path.join(outDir, n + ".json"), out + "\n");
  }
} else {
  let mode = "auto", useJsx = false, file = null;
  for (const a of args) {
    if (a === "--module") mode = "module";
    else if (a === "--script") mode = "script";
    else if (a === "--auto") mode = "auto";
    else if (a === "--jsx") useJsx = true;
    else file = a;
  }
  process.stdout.write(toJson(fs.readFileSync(file, "utf8"), mode, useJsx) + "\n");
}

// mep's Tests panel reporter for Node's built-in runner (src/main.cpp's
// javascript test provider): `node --test --test-reporter=<this file>
// --test-reporter-destination=stdout`. One line per event, in the protocol
// src/test_runner.h parses, with each file as one test named by its path
// relative to the project:
//
//   @@mep-test start FILE
//   @@mep-test result STATUS SECONDS FILE     (passed, failed or skipped)
//
// and what failed in it just before its result line. A file ends at its own
// test:summary (Node 22+ sends one per file); anything still open when the
// stream ends is reported then.

import path from 'node:path';

export default async function* mepReporter(source) {
  const root = process.cwd();
  const files = new Map();
  const name = (file) => path.relative(root, file).split(path.sep).join('/');
  const open = (file) => {
    let f = files.get(file);
    if (!f) {
      f = { failed: false, passed: 0, skipped: 0, lines: [], t0: Date.now(), done: false };
      files.set(file, f);
    }
    return f;
  };
  const start = (file) => {
    if (files.has(file)) return '';
    open(file);
    return '\n@@mep-test start ' + name(file) + '\n';
  };
  const finish = (file) => {
    const f = files.get(file);
    if (!f || f.done) return '';
    f.done = true;
    const status = f.failed ? 'failed' : f.passed === 0 && f.skipped > 0 ? 'skipped' : 'passed';
    const text = f.lines.join('\n').replace(/\s+$/, '');
    const secs = ((Date.now() - f.t0) / 1000).toFixed(3);
    return (text ? text + '\n' : '') + '\n@@mep-test result ' + status + ' ' + secs + ' ' + name(file) + '\n';
  };

  for await (const event of source) {
    const data = event.data || {};
    const file = data.file;
    if (!file) continue;
    switch (event.type) {
      case 'test:start':
      case 'test:enqueue':
        yield start(file);
        break;
      case 'test:pass': {
        yield start(file);
        const f = open(file);
        if (data.skip || data.todo) f.skipped++;
        else if (data.details && data.details.type !== 'suite') f.passed++;
        break;
      }
      case 'test:fail': {
        yield start(file);
        const f = open(file);
        // A failing suite repeats its failing tests' errors; report the tests.
        if (data.details && data.details.type === 'suite') break;
        f.failed = true;
        const err = data.details && data.details.error;
        const cause = err && err.cause ? err.cause : err;
        const text = String((cause && (cause.stack || cause.message)) || cause || 'failed');
        // Frames inside Node's own runner say nothing about the test.
        const own = text.split('\n').filter((l) => !/^\s+at .*\(node:/.test(l));
        f.lines.push('● ' + data.name + '\n' + own.join('\n'));
        break;
      }
      case 'test:stderr':
      case 'test:stdout':
        if (data.message) open(file).lines.push(String(data.message).replace(/\n$/, ''));
        break;
      case 'test:summary':
        yield finish(file);
        break;
      default:
        break;
    }
  }
  for (const file of files.keys()) yield finish(file);
}

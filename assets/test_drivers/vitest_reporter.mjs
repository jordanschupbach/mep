// mep's Tests panel reporter for Vitest (src/main.cpp's javascript test
// provider): `vitest run --reporter=<this file>`. One line per event, in the
// protocol src/test_runner.h parses, with each file as one test named by its
// path relative to the project:
//
//   @@mep-test start FILE
//   @@mep-test result STATUS SECONDS FILE     (passed, failed or skipped)
//
// and what failed in it just before its result line. Vitest 3+ reports each
// file as it starts and ends (onTestModuleStart/End); older versions only
// hand over every file at the end (onFinished), which is reported then.

import path from 'node:path';

// eslint-disable-next-line no-control-regex
const ANSI = /\u001b\[[0-9;]*m/g;

function errorText(errors) {
  return (errors || [])
    .map((e) => (e && (e.stack || e.message)) || String(e))
    .join('\n')
    .replace(ANSI, '')
    // Frames inside Vitest itself say nothing about the test.
    .split('\n')
    .filter((l) => !/^\s+at (.*\/node_modules\/|new Promise \(<anonymous>\))/.test(l))
    .join('\n');
}

export default class MepReporter {
  constructor() {
    this.root = process.cwd();
    this.started = new Set();
    this.done = new Set();
  }

  onInit(ctx) {
    const root = ctx && ((ctx.config && ctx.config.root) || ctx.root);
    if (root) this.root = root;
  }

  name(file) {
    return path.relative(this.root, file).split(path.sep).join('/');
  }

  start(name) {
    if (this.started.has(name)) return;
    this.started.add(name);
    process.stdout.write('\n@@mep-test start ' + name + '\n');
  }

  finish(name, status, secs, text) {
    if (this.done.has(name)) return;
    this.done.add(name);
    this.start(name);
    text = (text || '').replace(/\s+$/, '');
    if (text) process.stdout.write(text + '\n');
    process.stdout.write('\n@@mep-test result ' + status + ' ' + (secs || 0).toFixed(3) + ' ' + name + '\n');
  }

  // Vitest 3+.
  onTestModuleStart(mod) {
    this.start(this.name(mod.moduleId));
  }

  onTestModuleEnd(mod) {
    const state = mod.state();
    const lines = [];
    const moduleErrors = errorText(mod.errors());
    if (moduleErrors) lines.push(moduleErrors);
    for (const test of mod.children.allTests()) {
      const r = test.result();
      if (r && r.state === 'failed') lines.push('● ' + test.fullName + '\n' + errorText(r.errors));
    }
    const status = state === 'failed' ? 'failed' : state === 'skipped' ? 'skipped' : 'passed';
    const d = mod.diagnostic ? mod.diagnostic() : null;
    this.finish(this.name(mod.moduleId), status, d ? d.duration / 1000 : 0, lines.join('\n\n'));
  }

  // Vitest 1 and 2 (and a safety net for any file the hooks above missed).
  onFinished(files) {
    for (const file of files || []) {
      const name = this.name(file.filepath);
      const lines = [];
      const own = errorText(file.result && file.result.errors);
      if (own) lines.push(own);
      const walk = (task, prefix) => {
        for (const t of task.tasks || []) {
          const full = prefix ? prefix + ' > ' + t.name : t.name;
          if (t.type === 'test' && t.result && t.result.state === 'fail') lines.push('● ' + full + '\n' + errorText(t.result.errors));
          walk(t, full);
        }
      };
      walk(file, '');
      const state = file.result && file.result.state;
      const status = state === 'fail' ? 'failed' : state === 'skip' || state === 'todo' ? 'skipped' : 'passed';
      this.finish(name, status, file.result && file.result.duration ? file.result.duration / 1000 : 0, lines.join('\n\n'));
    }
  }
}

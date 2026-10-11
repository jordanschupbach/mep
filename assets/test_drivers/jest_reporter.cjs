// mep's Tests panel reporter for Jest (src/main.cpp's javascript test
// provider): `jest --reporters=<this file>`. One line per event, in the
// protocol src/test_runner.h parses, with each file as one test named by
// its path relative to the project:
//
//   @@mep-test start FILE
//   @@mep-test result STATUS SECONDS FILE     (passed, failed or skipped)
//
// and what failed in it (Jest's own failure message) just before its result
// line. Jest runs files in parallel; a result line claims the lines since
// the marker before it, which is that file's own message.

const path = require('path');

// eslint-disable-next-line no-control-regex
const ANSI = /\u001b\[[0-9;]*m/g;

class MepReporter {
  // Names are relative to where Jest was started -- the project, which is
  // also what the panel's --listTests listing is made relative to (a config's
  // rootDir may point elsewhere).
  constructor() {
    this.root = process.cwd();
    this.started = new Set();
    this.done = new Set();
  }

  name(test) {
    return path.relative(this.root, test.path).split(path.sep).join('/');
  }

  write(text) {
    process.stdout.write(text);
  }

  start(test) {
    const name = this.name(test);
    if (this.started.has(name)) return;
    this.started.add(name);
    this.write('\n@@mep-test start ' + name + '\n');
  }

  onTestFileStart(test) {
    this.start(test);
  }

  onTestStart(test) {
    this.start(test);
  }

  onTestFileResult(test, result) {
    this.finish(test, result);
  }

  onTestResult(test, result) {
    this.finish(test, result);
  }

  finish(test, result) {
    const name = this.name(test);
    if (!this.started.has(name)) this.start(test);
    // (Jest 27+ calls both onTestFileResult and the older onTestResult.)
    if (this.done.has(name)) return;
    this.done.add(name);
    let status = 'passed';
    if (result.testExecError || result.numFailingTests > 0 || result.failureMessage) status = 'failed';
    else if (result.numPassingTests === 0 && (result.numPendingTests > 0 || result.numTodoTests > 0)) status = 'skipped';
    let text = result.failureMessage || (result.testExecError && result.testExecError.message) || '';
    text = text.replace(ANSI, '').replace(/\s+$/, '');
    if (text) this.write(text + '\n');
    const stats = result.perfStats || {};
    const secs = stats.end && stats.start ? (stats.end - stats.start) / 1000 : 0;
    this.write('\n@@mep-test result ' + status + ' ' + secs.toFixed(3) + ' ' + name + '\n');
  }

  getLastError() {
    return undefined;
  }
}

module.exports = MepReporter;

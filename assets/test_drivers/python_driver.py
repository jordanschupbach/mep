# mep's Tests panel driver for Python (src/main.cpp's python test provider).
#
#   python3 python_driver.py list ROOT RUNNER
#   python3 python_driver.py run ROOT RUNNER [ID ...]
#
# RUNNER is auto (pytest when it imports, else unittest), pytest or
# unittest. Everything the panel reads is one line each, in the protocol
# src/test_runner.h parses:
#
#   @@mep-test runner NAME
#   @@mep-test case ID
#   @@mep-test start ID
#   @@mep-test result STATUS SECONDS ID      (passed, failed or skipped)
#
# with a test's own output (its traceback and anything it printed) between
# its start and result lines. Anything else the runner prints is left alone.

import os
import sys
import time


# The real stdout: unittest's buffering swaps sys.stdout out while a test runs.
OUT = sys.__stdout__


def mark(*parts):
    # A leading newline, since a runner's progress dots may be pending.
    OUT.write("\n@@mep-test " + " ".join(parts) + "\n")
    OUT.flush()


def show(text):
    text = (text or "").rstrip()
    if text:
        OUT.write(text + "\n")


def run_pytest(mode, root, ids):
    import pytest

    class Plugin:
        def __init__(self):
            self.t0 = {}
            self.status = {}
            self.output = {}

        # pytest's own reporter would interleave its progress characters
        # with the lines above. It goes once the options are parsed, so a
        # project's addopts (-q, --tb, -ra ...) still parse; -p no:terminal
        # would make those unknown options.
        @pytest.hookimpl(trylast=True)
        def pytest_configure(self, config):
            reporter = config.pluginmanager.getplugin("terminalreporter")
            if reporter is not None:
                config.pluginmanager.unregister(reporter)

        def pytest_collection_finish(self, session):
            if mode == "list":
                for item in session.items:
                    mark("case", item.nodeid)

        # A file that does not even import is a failing row of its own,
        # named by its path, rather than a run that stops before it starts.
        def pytest_collectreport(self, report):
            if not report.failed or not report.nodeid:
                return
            mark("case", report.nodeid)
            if mode == "run":
                mark("start", report.nodeid)
                show(report.longreprtext)
                mark("result", "failed", "0.000", report.nodeid)

        def pytest_runtest_logstart(self, nodeid, location):
            self.t0[nodeid] = time.monotonic()
            self.status[nodeid] = "passed"
            self.output[nodeid] = []
            mark("start", nodeid)

        def pytest_runtest_logreport(self, report):
            nid = report.nodeid
            if report.failed:
                self.status[nid] = "failed"
                self.output.setdefault(nid, []).append(report.longreprtext)
                for name, content in report.sections:
                    self.output[nid].append("----- " + name + " -----\n" + content)
            elif report.skipped and report.when in ("setup", "call") and self.status.get(nid) != "failed":
                self.status[nid] = "skipped"

        def pytest_runtest_logfinish(self, nodeid, location):
            for text in self.output.get(nodeid, []):
                show(text)
            secs = time.monotonic() - self.t0.get(nodeid, time.monotonic())
            mark("result", self.status.get(nodeid, "passed"), "%.3f" % secs, nodeid)

    args = ["--rootdir", root, "-q", "-p", "no:cacheprovider", "--continue-on-collection-errors"]
    if mode == "list":
        args.append("--collect-only")
    else:
        args += ["--tb=short", "-rN"]
    return pytest.main(args + list(ids), plugins=[Plugin()])


def unittest_dirs(root):
    # discover() only recurses into packages, so a tests/ directory without
    # an __init__.py is its own top level.
    for name in ("tests", "test"):
        d = os.path.join(root, name)
        if os.path.isdir(d):
            top = root if os.path.isfile(os.path.join(d, "__init__.py")) else d
            return d, top
    return root, root


def run_unittest(mode, root, ids):
    import unittest

    start, top = unittest_dirs(root)
    for p in (root, top):
        if p not in sys.path:
            sys.path.insert(0, p)
    loader = unittest.TestLoader()

    def flatten(suite):
        for t in suite:
            if isinstance(t, unittest.TestSuite):
                yield from flatten(t)
            else:
                yield t

    if mode == "list":
        for t in flatten(loader.discover(start, top_level_dir=top)):
            mark("case", t.id())
        return 0

    class Result(unittest.TestResult):
        def startTest(self, test):
            super().startTest(test)
            self.t0 = time.monotonic()
            self.status = "passed"
            self.lines = []
            mark("start", test.id())

        def _fail(self, test, err):
            self.status = "failed"
            self.lines.append(self._exc_info_to_string(err, test))

        def addFailure(self, test, err):
            super().addFailure(test, err)
            self._fail(test, err)

        def addError(self, test, err):
            super().addError(test, err)
            self._fail(test, err)

        def addSubTest(self, test, subtest, err):
            super().addSubTest(test, subtest, err)
            if err is not None:
                self.lines.append(str(subtest))
                self._fail(test, err)

        def addSkip(self, test, reason):
            super().addSkip(test, reason)
            self.status = "skipped"
            self.lines.append("skipped: " + reason)

        def addUnexpectedSuccess(self, test):
            super().addUnexpectedSuccess(test)
            self.status = "failed"
            self.lines.append("unexpected success")

        def stopTest(self, test):
            # The captured output is already in the traceback text; don't
            # echo it a second time.
            self._mirrorOutput = False
            super().stopTest(test)
            for text in self.lines:
                show(text)
            mark("result", self.status, "%.3f" % (time.monotonic() - self.t0), test.id())

    os.chdir(top)
    if ids:
        suite = unittest.TestSuite()
        for i in ids:
            suite.addTests(loader.loadTestsFromName(i))
    else:
        suite = loader.discover(start, top_level_dir=top)
    result = Result()
    result.buffer = True
    suite.run(result)
    return 0 if result.wasSuccessful() else 1


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ("list", "run"):
        sys.stderr.write("usage: python_driver.py list|run ROOT auto|pytest|unittest [ID ...]\n")
        return 2
    mode, root, runner, ids = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3], sys.argv[4:]
    if runner == "auto":
        try:
            import pytest  # noqa: F401
            runner = "pytest"
        except ImportError:
            runner = "unittest"
    mark("runner", runner)
    os.chdir(root)
    if runner == "pytest":
        return int(run_pytest(mode, root, ids))
    return run_unittest(mode, root, ids)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Exercise installed-package hover help over the real R LSP protocol.

Requires R and Rscript. Builds temporary local help fixtures; installs nothing
into the user's libraries. Usage: python3 tools/test_r_lsp_help.py [SERVER]
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def frame(message):
    body = json.dumps(message).encode()
    return f'Content-Length: {len(body)}\r\n\r\n'.encode() + body


def responses(data):
    result = {}
    while data:
        header, data = data.split(b'\r\n\r\n', 1)
        size = int(header.split(b':', 1)[1])
        message = json.loads(data[:size])
        data = data[size:]
        if 'id' in message:
            assert 'error' not in message, message
            result[message['id']] = message['result']
    return result


def run(server, documents, requests, env):
    messages = [{'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {}}]
    for uri, text in documents.items():
        messages.append({'jsonrpc': '2.0', 'method': 'textDocument/didOpen',
                         'params': {'textDocument': {'uri': uri, 'languageId': 'r', 'version': 1, 'text': text}}})
    for i, (uri, line, col) in enumerate(requests, 10):
        messages.append({'jsonrpc': '2.0', 'id': i, 'method': 'textDocument/hover',
                         'params': {'textDocument': {'uri': uri}, 'position': {'line': line, 'character': col}}})
    messages.extend([{'jsonrpc': '2.0', 'id': 2, 'method': 'shutdown'},
                     {'jsonrpc': '2.0', 'method': 'exit'}])
    process = subprocess.run([str(server)], input=b''.join(map(frame, messages)), env=env,
                             capture_output=True, timeout=30)
    assert process.returncode == 0, process.stderr.decode()
    result = responses(process.stdout)
    return [result[i] for i in range(10, 10 + len(requests))]


def install_fixture(root, lib, package, title):
    source = root / package
    (source / 'R').mkdir(parents=True)
    (source / 'man').mkdir()
    (source / 'DESCRIPTION').write_text(f'''Package: {package}
Version: 1.0
Title: Hover Test Fixture
Description: Documentation lookup regression fixture.
Author: Test
Maintainer: Test <test@example.invalid>
License: MIT
''')
    (source / 'NAMESPACE').write_text('export(widget.fn, mean)\n')
    (source / 'R/fixture.R').write_text('''widget.fn <- function(x = 1) x
mean <- function(x) x
.onLoad <- function(...) {
  writeLines("loaded", Sys.getenv("MEP_R_TEST_LOAD_MARKER"))
  stop("Help lookup must not load this package")
}
''')
    (source / 'man/widget.Rd').write_text(r'\name{widget}' + '\n' +
        r'\alias{widget.fn}' + '\n' + r'\alias{mean}' + '\n' +
        r'\title{' + title + '}\n' + r'\description{Read installed documentation without loading the package.}' + '\n' +
        r'\usage{widget.fn(x = 1)}' + '\n' + r'\arguments{\item{x}{A value.}}' + '\n')
    process = subprocess.run(['R', 'CMD', 'INSTALL', '--no-test-load', '--no-byte-compile',
                              '--no-staged-install', '--library=' + str(lib), str(source)],
                             capture_output=True, text=True, timeout=30)
    assert process.returncode == 0, process.stdout + process.stderr


def main():
    server = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/native/mep-r-lsp').resolve()
    rscript = shutil.which('Rscript')
    assert rscript and shutil.which('R'), 'R and Rscript must be on PATH'
    with tempfile.TemporaryDirectory(prefix='mep-r-help-') as directory:
        root = Path(directory)
        lib = root / 'library'
        lib.mkdir()
        install_fixture(root, lib, 'mephoverfixture', 'First fixture package documentation')
        install_fixture(root, lib, 'mephoversecond', 'Second fixture package documentation')
        env = dict(os.environ, R_LIBS_USER=str(lib), MEP_R_TEST_LOAD_MARKER=str(root / 'loaded'),
                   MEP_RSCRIPT=rscript)
        launches = root / 'launches'
        if os.name != 'nt':
            wrapper = root / 'Rscript with spaces'
            wrapper.write_text(f'#!{sys.executable}\nimport os, sys\n'
                               f'with open({str(launches)!r}, "a") as f: f.write("lookup\\n")\n'
                               f'os.execv({rscript!r}, [{rscript!r}] + sys.argv[1:])\n')
            wrapper.chmod(0o755)
            env['MEP_RSCRIPT'] = str(wrapper)
        example_uri = (root / 'r_example.r').as_uri()
        fixture_uri = (root / 'fixture.r').as_uri()
        plain_uri = (root / 'plain.r').as_uri()
        documents = {
            example_uri: (ROOT / 'examples/language/r_example.r').read_text(),
            fixture_uri: '\n'.join([
                'library(mephoverfixture)', 'widget.fn(1)', 'mephoverfixture::widget.fn(1)',
                'mephoverfixture::mean(1)', 'missing.package::widget.fn(1)',
                'obj$widget.fn', '# widget.fn', '"widget.fn"',
                'local.fn <- function(x) x', 'local.fn(1)',
                'base::mean(1)', 'splines:::spline.des(knots, x)',
            ]),
            plain_uri: 'widget.fn(1)\nspline.des(knots, x)\nmissing.mep.topic()\n',
        }
        requests = [(example_uri, 10, col) for col in (8, 14, 17)]
        requests += [(fixture_uri, line, col) for line, col in
                     [(1, 4), (2, 25), (3, 20), (4, 22), (5, 8), (6, 5), (7, 4), (9, 4), (10, 7), (11, 18)]]
        requests += [(plain_uri, 0, 4), (plain_uri, 1, 8), (plain_uri, 2, 5), (plain_uri, 2, 5)]
        results = run(server, documents, requests, env)
        for result in results[:3]:
            text = result['contents']['value']
            assert 'Package: splines' in text and 'Design Matrix for B-splines' in text, result
            assert result['range']['start']['character'] == 8, result
            assert result['range']['end']['character'] == 18, result
            assert '\b' not in text, 'terminal backspaces leaked into the popup'
            assert "'knots'" in text, 'help should use readable plain quotes'
        for index in (3, 4, 5):
            assert 'First fixture package documentation' in results[index]['contents']['value'], results[index]
        assert all(result is None for result in results[6:10]), results[6:10]
        assert 'Defined' in results[10]['contents']['value'], results[10]
        assert 'Package: base' in results[11]['contents']['value'], results[11]
        assert 'Package: splines' in results[12]['contents']['value'], results[12]
        assert 'multiple packages' in results[13]['contents']['value'], results[13]
        assert 'mephoverfixture::widget.fn' in results[13]['contents']['value'], results[13]
        assert 'Package: splines' in results[14]['contents']['value'], results[14]
        assert results[15:] == [None, None], results[15:]
        assert not (root / 'loaded').exists(), 'package .onLoad ran during documentation lookup'
        if launches.exists():
            assert len(launches.read_text().splitlines()) == 9, launches.read_text()

        # A later library() must win when multiple attached packages document a topic.
        documents[fixture_uri] = 'library(mephoverfixture)\nlibrary(mephoversecond)\nwidget.fn(1)'
        result = run(server, documents, [(fixture_uri, 2, 4)], env)[0]
        assert 'Second fixture package documentation' in result['contents']['value'], result
        env['MEP_RSCRIPT'] = str(root / 'does-not-exist')
        unavailable = run(server, documents, [(example_uri, 10, 14), (fixture_uri, 2, 4),
                                             (example_uri, 4, 7)], env)
        assert unavailable[:2] == [None, None], unavailable
        assert 'runif(' in unavailable[2]['contents']['value'], unavailable
        if os.name != 'nt':
            sleeper = root / 'slow-rscript'
            sleeper.write_text(f'#!{sys.executable}\nimport time\ntime.sleep(15)\n')
            sleeper.chmod(0o755)
            env['MEP_RSCRIPT'] = str(sleeper)
            start = time.monotonic()
            timed_out = run(server, documents, [(example_uri, 10, 14), (example_uri, 10, 14)], env)
            assert timed_out == [None, None]
            assert 4 <= time.monotonic() - start < 8, 'lookup timeout or failure cache did not work'
    print('R installed-package help, dotted names, aliases, namespaces, caching, and failure checks passed.')


if __name__ == '__main__':
    main()

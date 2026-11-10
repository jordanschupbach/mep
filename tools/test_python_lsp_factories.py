#!/usr/bin/env python3
"""Exercise return-type introspection over LSP without third-party dependencies.

Run after building mep-python-lsp:
    python3 tools/test_python_lsp_factories.py build/native/mep-python-lsp
"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap


def frame(message):
    body = json.dumps(message).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


def responses(data):
    result = {}
    while data:
        header, data = data.split(b"\r\n\r\n", 1)
        length = int(header.split(b":", 1)[1])
        message = json.loads(data[:length])
        data = data[length:]
        if "id" in message:
            if "error" in message:
                raise AssertionError(message)
            result[message["id"]] = message["result"]
    return result


def main():
    server = Path(sys.argv[1] if len(sys.argv) > 1 else "build/native/mep-python-lsp").resolve()
    with tempfile.TemporaryDirectory(prefix="mep-python-lsp-") as directory:
        root = Path(directory)
        (root / "factory_fixture.py").write_text(textwrap.dedent('''\
            from __future__ import annotations

            class Figure:
                def savefig(self, filename):
                    """Save the fixture figure."""

            class Axes:
                def __init__(self):
                    raise AssertionError("LSP must not instantiate a class")
                def set_title(self, label):
                    """Set the fixture title."""

            def annotated() -> tuple[Figure, Axes]:
                raise AssertionError("LSP must not call a factory")

            def documented():
                """Build a figure and axes.

                Returns
                -------
                figure : Figure
                    The figure.
                axes : Axes
                    The axes.
                """
                raise AssertionError("LSP must not call a factory")

            def unknown():
                raise AssertionError("LSP must not call an unknown factory")

            def malicious() -> "__import__('pathlib').Path('executed').touch()":
                raise AssertionError("LSP must not evaluate annotations")
        '''))
        lines = [
            "import factory_fixture as lib",
            "(fig, ax) = lib.annotated()",
            "ax.set_title('plot')",
            "fig.savefig('plot.png')",
            "figure, axes = lib.documented()",
            "axes.set_title('plot')",
            "direct = lib.Axes()",
            "direct.set_title('plot')",
            "missing = lib.unknown()",
            "missing.set_title('plot')",
            "bad = lib.malicious()",
            "bad.set_title('plot')",
            "ax.",
        ]
        hover_lines = [2, 3, 5, 7, 9, 11]
        if sys.version_info >= (3, 14):
            (root / "deferred_fixture.py").write_text(textwrap.dedent('''\
                def factory() -> __import__('pathlib').Path('executed').touch():
                    raise AssertionError("LSP must not evaluate deferred annotations")
            '''))
            lines.extend(["import deferred_fixture as deferred", "deferred_value = deferred.factory()",
                          "deferred_value.set_title('plot')"])
            hover_lines.append(15)
        uri = (root / "example.py").as_uri()
        messages = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"initializationOptions": {"pythonPath": sys.executable}}},
            {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "python", "version": 1,
                                 "text": "\n".join(lines)}}},
        ]
        for line in hover_lines:
            messages.append({"jsonrpc": "2.0", "id": line + 10, "method": "textDocument/hover",
                             "params": {"textDocument": {"uri": uri},
                                        "position": {"line": line, "character": lines[line].index(".") + 2}}})
        messages.extend([
            {"jsonrpc": "2.0", "id": 30, "method": "textDocument/completion", "params": {
                "textDocument": {"uri": uri}, "position": {"line": 12, "character": 3}}},
            {"jsonrpc": "2.0", "id": 99, "method": "shutdown"},
            {"jsonrpc": "2.0", "method": "exit"},
        ])
        run = subprocess.run([str(server)], input=b"".join(map(frame, messages)),
                             capture_output=True, timeout=60, check=True)
        replies = responses(run.stdout)
        for line in (2, 5, 7):
            assert "Set the fixture title." in replies[line + 10]["contents"]["value"], replies[line + 10]
        assert "Save the fixture figure." in replies[13]["contents"]["value"]
        assert replies[19] is None, replies[19]
        assert replies[21] is None, replies[21]
        if sys.version_info >= (3, 14):
            assert replies[25] is None, replies[25]
        assert any(item["label"] == "set_title" for item in replies[30]["items"])
        assert not (root / "executed").exists(), "An annotation was executed"
    print("Python LSP factory hover/completion integration checks passed.")


if __name__ == "__main__":
    main()

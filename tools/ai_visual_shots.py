#!/usr/bin/env python3
"""Screenshots of Visual-mode `K` in a text buffer: highlight, ask, answer.

`K` on a Visual selection is mep.ai_send_selection: the selected text is
the whole prompt, and the reply is streamed into the buffer at the cursor
with mep.insert_text, so the answer lands inline next to the code it is
about.

Driven headlessly, with the model API pointed at a local HTTP sink that
returns a canned reply -- so these are real screenshots of the real code
path, with no network call, no API key and no tokens spent. The replies
are written here rather than generated, which is the point: what is being
shown is mep's selection, dispatch and inline streaming, not a model's
prose.

Reuses the Mep driver and WireSink from pdf_live_check.py.

Usage: tools/ai_visual_shots.py [--out DIR] [--keep]
"""

import argparse
import http.server
import json
import os
import shutil
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pdf_live_check import Mep  # noqa: E402


class ScriptedSink:
    """A local stand-in for the chat API that streams a fixed reply.

    mep shells out to `curl -X POST <base_url>/chat/completions`, so
    pointing its base URL here exercises the whole client -- SSE parsing
    included -- without leaving the machine. The reply is delivered in
    small chunks with a pause between them, because the inline insert
    happens per chunk and streaming it is what the screenshots are of.
    """

    def __init__(self):
        self.reply = ""
        self.requests = []
        sink = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                sink.requests.append(self.rfile.read(n))
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                # Chunked by hand so the client sees a real stream.
                text = sink.reply
                step = 24
                for i in range(0, len(text), step):
                    piece = json.dumps({"choices": [{"delta": {"content": text[i:i + step]}}]})
                    body = ("data: %s\n\n" % piece).encode()
                    self.wfile.write(b"%X\r\n" % len(body) + body + b"\r\n")
                    self.wfile.flush()
                    time.sleep(0.04)
                tail = b"data: [DONE]\n\n"
                self.wfile.write(b"%X\r\n" % len(tail) + tail + b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()

            def log_message(self, *a):
                pass

        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


# Two scenarios: (filename, contents, (first line, extra lines to
# extend over), canned reply). Selected by line number because
# `command.run` takes ex-commands -- `:N` is a line jump, while
# `/pattern` is not an ex-command at all and silently moves nothing.
SCENARIOS = [
    (
        "explain.py",
        '''# Visual-mode K sends the highlighted text to the model and streams
# the reply back into the buffer, right here at the cursor.


def reservoir_sample(stream, k):
    chosen = []
    for i, item in enumerate(stream):
        if i < k:
            chosen.append(item)
        else:
            j = random.randrange(i + 1)
            if j < k:
                chosen[j] = item
    return chosen


# <- the explanation lands here
''',
        (5, 9),   # the function body: :5, then V and nine j's
        "\n"
        "# Keeps k items uniformly at random from a stream of unknown length.\n"
        "# The first k go in directly; after that item i is kept with\n"
        "# probability k/(i+1), replacing a uniformly chosen incumbent.\n"
        "# One pass, O(k) memory, never needs len(stream).\n",
    ),
    (
        "write.c",
        '''/* Highlight the comment below and press K: the model writes the
   function underneath it, streamed in as it arrives. */

/* clamp_add: add two ints, saturating at INT_MIN/INT_MAX instead of
   overflowing. Write it. */


/* <- the code lands here */
''',
        (4, 1),   # the spec comment: :4, then V and one j
        "\n"
        "int clamp_add(int a, int b) {\n"
        "    if (b > 0 && a > INT_MAX - b) return INT_MAX;\n"
        "    if (b < 0 && a < INT_MIN - b) return INT_MIN;\n"
        "    return a + b;\n"
        "}\n",
    ),
]


def shoot(m, out_dir, name):
    """One screenshot, copied out of mep's own throwaway data dir."""
    path = m.rpc.call("ui.screenshot")
    src = path.get("path") if isinstance(path, dict) else path
    if not src or not os.path.exists(src):
        print("   !! screenshot failed (%r)" % (path,))
        return None
    dst = os.path.join(out_dir, name)
    shutil.copyfile(src, dst)
    print("   wrote %s" % dst)
    return dst


def run(build_dir, out_dir, keep):
    os.makedirs(out_dir, exist_ok=True)
    shots = []
    for fname, body, (first_line, extend), reply in SCENARIOS:
        sink = ScriptedSink()
        sink.reply = reply
        work = os.path.join(out_dir, "src")
        os.makedirs(work, exist_ok=True)
        path = os.path.join(work, fname)
        with open(path, "w") as fh:
            fh.write(body)

        m = Mep(build_dir, path, keep=keep)
        try:
            print("== %s" % fname)
            base = "http://127.0.0.1:%d" % sink.port
            m.cmd("lua mep.ai_api_key = 'local-sink'")
            m.cmd("lua mep.ai_provider = 'openai'")
            m.cmd("lua mep.ai_base_url = %s" % json.dumps(base + "/v1"))
            m.cmd("lua mep.ai_http_enabled = true")
            m.pump(12)

            # Cursor to the first line of the construct, then select it
            # linewise: V, then j for each further line.
            m.cmd(str(first_line))
            m.pump(6)
            m.shift_key("v")
            for _ in range(extend):
                m.key("j")
            m.pump(8)
            shots.append(shoot(m, out_dir, fname.replace(".", "-") + "-1-highlighted.png"))

            # K: send the selection. Shift must genuinely be held --
            # ui.key_press goes straight to the X keysym with no shift
            # handling of its own.
            m.shift_key("k")
            # The reply streams in a chunk at a time and each chunk is
            # inserted on a frame, so this needs many frames to drain.
            for _ in range(40):
                m.pump(6)
                time.sleep(0.1)
            shots.append(shoot(m, out_dir, fname.replace(".", "-") + "-2-answered.png"))
            print("   request bodies captured: %d" % len(sink.requests))
            if sink.requests:
                sent = json.loads(sink.requests[0].decode("utf-8", "replace"))
                content = (sent.get("messages") or [{}])[0].get("content", "")
                print("   prompt was the selection: %r" % content[:90])
        finally:
            m.close()
            sink.close()
    return [s for s in shots if s]


def main():
    ap = argparse.ArgumentParser()
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--build-dir", default=os.path.join(repo, "build", "native-dev"))
    ap.add_argument("--out", default=os.path.join(repo, "build", "ai-shots"))
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    if shutil.which("Xvfb") is None:
        sys.exit("Xvfb not on PATH")
    shots = run(a.build_dir, a.out, a.keep)
    print("\n%d screenshots in %s" % (len(shots), a.out))


if __name__ == "__main__":
    main()

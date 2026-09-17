// Windowless test for notebook_doc.cpp: nbformat parse/serialize round
// trips, the percent-format text view, cell re-matching after edits, the
// output display/slot helpers and the kernel wire protocol. CHECK(),
// never assert(): the Release build strips assert() entirely.
#include "notebook_doc.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

// A 1x1 transparent PNG (68 bytes) -- enough for the IHDR reader.
const char *kTinyPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

const char *kSample = R"({
 "cells": [
  {
   "cell_type": "markdown",
   "id": "aaa11111",
   "metadata": {},
   "source": [
    "# Title\n",
    "\n",
    "Some *prose*."
   ]
  },
  {
   "cell_type": "code",
   "execution_count": 3,
   "id": "bbb22222",
   "metadata": {
    "tags": [
     "keep"
    ]
   },
   "outputs": [
    {
     "name": "stdout",
     "output_type": "stream",
     "text": [
      "hello\n",
      "world\n"
     ]
    },
    {
     "data": {
      "text/plain": [
       "42"
      ]
     },
     "execution_count": 3,
     "metadata": {},
     "output_type": "execute_result"
    }
   ],
   "source": [
    "print('hello')\n",
    "print('world')\n",
    "42"
   ]
  },
  {
   "cell_type": "code",
   "execution_count": null,
   "id": "ccc33333",
   "metadata": {},
   "outputs": [],
   "source": []
  }
 ],
 "metadata": {
  "kernelspec": {
   "display_name": "Python 3",
   "language": "python",
   "name": "python3"
  },
  "language_info": {
   "name": "python",
   "version": "3.12.0"
  }
 },
 "nbformat": 4,
 "nbformat_minor": 5
}
)";
}  // namespace

int main() {
    // --- Extension check
    {
        CHECK(IsIpynbPath("a/b/notebook.ipynb"));
        CHECK(IsIpynbPath("X.IPYNB"));
        CHECK(!IsIpynbPath("notebook.py"));
        CHECK(!IsIpynbPath("dir.ipynb/file"));
    }

    // --- Parse: cell types, sources, execution counts, outputs, metadata
    NotebookDoc doc;
    {
        std::string err;
        CHECK(ParseNotebook(kSample, &doc, &err));
        CHECK(doc.cells.size() == 3);
        CHECK(doc.nbformat == 4 && doc.nbformat_minor == 5);
        CHECK(doc.cells[0].type == NotebookCellType::Markdown);
        CHECK(doc.cells[0].id == "aaa11111");
        CHECK(doc.cells[0].source == "# Title\n\nSome *prose*.");
        CHECK(doc.cells[1].type == NotebookCellType::Code);
        CHECK(doc.cells[1].execution_count == 3);
        CHECK(doc.cells[1].source == "print('hello')\nprint('world')\n42");
        CHECK(doc.cells[1].outputs.size() == 2);
        CHECK(doc.cells[1].outputs[0].kind == NotebookOutput::Kind::Stream);
        CHECK(doc.cells[1].outputs[0].name == "stdout");
        CHECK(doc.cells[1].outputs[0].text == "hello\nworld\n");
        CHECK(doc.cells[1].outputs[1].kind == NotebookOutput::Kind::ExecuteResult);
        CHECK(doc.cells[1].outputs[1].text == "42");
        CHECK(doc.cells[1].outputs[1].execution_count == 3);
        CHECK(!doc.cells[1].outputs[1].raw.is_null());
        CHECK(doc.cells[1].metadata.get("tags").is_array());
        CHECK(doc.cells[2].execution_count == -1);
        CHECK(doc.cells[2].source.empty());
        CHECK(doc.metadata.get("kernelspec").get("name").as_string() == "python3");

        NotebookDoc bad;
        CHECK(!ParseNotebook("{\"nbformat\": 4}", &bad, &err));
        CHECK(!err.empty());
        CHECK(!ParseNotebook("not json", &bad, &err));
    }

    // --- Serialize: byte-identical round trip of Jupyter's own layout
    {
        std::string out = SerializeNotebook(doc);
        CHECK(out == kSample);
        NotebookDoc again;
        CHECK(ParseNotebook(out, &again, nullptr));
        CHECK(again.cells.size() == 3);
        CHECK(again.cells[1].outputs[0].text == "hello\nworld\n");
    }

    // --- Text view: markers + sources, blank separator, raw markdown
    {
        Lines lines = NotebookToLines(doc);
        const Lines expected = {
            "# %% [markdown]", "# Title", "", "Some *prose*.", "",
            "# %%", "print('hello')", "print('world')", "42", "",
            "# %%",
        };
        CHECK(lines == expected);

        NotebookCellType t = NotebookCellType::Raw;
        CHECK(ParseNotebookMarker("# %%", &t) && t == NotebookCellType::Code);
        CHECK(ParseNotebookMarker("  # %% [markdown] Title", &t) && t == NotebookCellType::Markdown);
        CHECK(ParseNotebookMarker("# %% [md]", &t) && t == NotebookCellType::Markdown);
        CHECK(ParseNotebookMarker("# %% [raw]", &t) && t == NotebookCellType::Raw);
        CHECK(!ParseNotebookMarker("# %%%", &t));
        CHECK(!ParseNotebookMarker("x = 1  # %%", &t));
        CHECK(!ParseNotebookMarker("#%%", &t));

        std::vector<NotebookCellSpan> spans = ScanNotebookCells(lines);
        CHECK(spans.size() == 3);
        CHECK(spans[0].marker_row == 0 && spans[0].first_row == 1 && spans[0].end_row == 5);
        CHECK(spans[0].type == NotebookCellType::Markdown);
        CHECK(spans[1].marker_row == 5 && spans[1].first_row == 6 && spans[1].end_row == 10);
        CHECK(spans[2].marker_row == 10 && spans[2].first_row == 11 && spans[2].end_row == 11);
        CHECK(NotebookSpanSource(lines, spans[0]) == "# Title\n\nSome *prose*.");
        CHECK(NotebookSpanSource(lines, spans[1]) == "print('hello')\nprint('world')\n42");
        CHECK(NotebookSpanSource(lines, spans[2]).empty());
        CHECK(NotebookSpanAtRow(spans, 0) == 0);
        CHECK(NotebookSpanAtRow(spans, 4) == 0);
        CHECK(NotebookSpanAtRow(spans, 5) == 1);
        CHECK(NotebookSpanAtRow(spans, 10) == 2);
        CHECK(NotebookSpanAtRow(spans, 11) == -1);

        // Implicit leading cell only when there's real content up front.
        const Lines lead = {"", "x = 1", "# %%", "y"};
        std::vector<NotebookCellSpan> lead_spans = ScanNotebookCells(lead);
        CHECK(lead_spans.size() == 2);
        CHECK(lead_spans[0].marker_row == -1 && lead_spans[0].first_row == 0 && lead_spans[0].end_row == 2);
        CHECK(NotebookSpanSource(lead, lead_spans[0]) == "\nx = 1");
        const Lines blank_lead = {"", "  ", "# %%", "y"};
        CHECK(ScanNotebookCells(blank_lead).size() == 1);
        CHECK(ScanNotebookCells(Lines{}).empty());

        NotebookDoc empty;
        CHECK(NotebookToLines(empty) == Lines{"# %%"});
    }

    // --- Sync, same cell count: in-place edit keeps outputs; type change drops them
    {
        NotebookDoc d = doc;
        int next_uid = 1;
        Lines lines = NotebookToLines(d);
        lines[6] = "print('HELLO')";
        std::vector<NotebookCellSpan> spans = ScanNotebookCells(lines);
        SyncNotebookFromLines(&d, lines, spans, &next_uid);
        CHECK(d.cells.size() == 3);
        CHECK(d.cells[1].source == "print('HELLO')\nprint('world')\n42");
        CHECK(d.cells[1].outputs.size() == 2);
        CHECK(d.cells[1].id == "bbb22222");
        CHECK(d.cells[0].uid == 1 && d.cells[1].uid == 2 && d.cells[2].uid == 3);

        lines[5] = "# %% [markdown]";
        spans = ScanNotebookCells(lines);
        SyncNotebookFromLines(&d, lines, spans, &next_uid);
        CHECK(d.cells[1].type == NotebookCellType::Markdown);
        CHECK(d.cells[1].outputs.empty());
        CHECK(d.cells[1].execution_count == -1);
    }

    // --- Sync, cell count changed: match by source, fresh cells for the rest
    {
        NotebookDoc d = doc;
        int next_uid = 10;
        Lines lines = NotebookToLines(d);
        SyncNotebookFromLines(&d, lines, ScanNotebookCells(lines), &next_uid);
        // Insert a new cell between the markdown and the code cell.
        lines.insert(lines.begin() + 5, {"# %%", "z = 0", ""});
        std::vector<NotebookCellSpan> spans = ScanNotebookCells(lines);
        CHECK(spans.size() == 4);
        SyncNotebookFromLines(&d, lines, spans, &next_uid);
        CHECK(d.cells.size() == 4);
        CHECK(d.cells[0].id == "aaa11111");
        CHECK(d.cells[1].source == "z = 0");
        CHECK(d.cells[1].outputs.empty());
        CHECK(d.cells[1].uid == 13);
        CHECK(!d.cells[1].id.empty() && d.cells[1].id.size() == 8);
        CHECK(d.cells[2].id == "bbb22222");
        CHECK(d.cells[2].outputs.size() == 2);
        CHECK(d.cells[2].uid == 11);
        CHECK(d.cells[3].id == "ccc33333");

        // Delete the first (markdown) cell: everything else keeps identity.
        lines.erase(lines.begin(), lines.begin() + 5);
        spans = ScanNotebookCells(lines);
        SyncNotebookFromLines(&d, lines, spans, &next_uid);
        CHECK(d.cells.size() == 3);
        CHECK(d.cells[0].source == "z = 0");
        CHECK(d.cells[1].id == "bbb22222" && d.cells[1].outputs.size() == 2);
        CHECK(d.cells[2].id == "ccc33333");
    }

    // --- Output display: ANSI stripping, trailing newline, truncation, slots
    {
        CHECK(StripAnsi("\x1b[31mred\x1b[0m plain\r\n") == "red plain\n");
        NotebookOutput s;
        s.kind = NotebookOutput::Kind::Stream;
        s.name = "stdout";
        s.text = "a\nb\n";
        Lines shown = NotebookOutputDisplayLines(s);
        const Lines expected_ab = {"a", "b"};
        CHECK(shown == expected_ab);
        CHECK(NotebookOutputSlots(s, 0.52) == 2);

        NotebookOutput blank;
        CHECK(NotebookOutputDisplayLines(blank).empty());
        CHECK(NotebookOutputSlots(blank, 0.52) == 0);

        NotebookOutput big;
        big.kind = NotebookOutput::Kind::Stream;
        for (int i = 0; i < 100; i++) big.text += "line\n";
        Lines big_shown = NotebookOutputDisplayLines(big);
        CHECK(static_cast<int>(big_shown.size()) == kNotebookMaxOutputLines + 1);
        CHECK(big_shown.back() == "... (60 more lines)");

        NotebookOutput err;
        err.kind = NotebookOutput::Kind::Error;
        err.text = "Traceback\nValueError: x";
        CHECK(NotebookOutputSlots(err, 0.52) == 2);

        int w = 0, h = 0;
        CHECK(PngDimensionsFromBase64(kTinyPng, &w, &h));
        CHECK(w == 1 && h == 1);
        CHECK(!PngDimensionsFromBase64("aGVsbG8=", &w, &h));
        CHECK(NotebookImageSlots(640, 480, 0.52) == 29);
        CHECK(NotebookImageSlots(640, 480, 0.38) == 21);
        CHECK(NotebookImageSlots(640, 480, 0.0) == 29);
        CHECK(NotebookImageSlots(1000, 100, 0.52) == 4);
        CHECK(NotebookImageSlots(100, 1000, 0.52) == kNotebookImageMaxSlots);
        CHECK(NotebookImageSlots(0, 0, 0.52) == 20);

        NotebookOutput img;
        img.kind = NotebookOutput::Kind::DisplayData;
        img.image_png = kTinyPng;
        img.image_width = 640;
        img.image_height = 480;
        img.text = "<Figure>";
        CHECK(NotebookOutputDisplayLines(img).empty());
        CHECK(NotebookOutputSlots(img, 0.52) == 29);

        NotebookCell cell;
        cell.type = NotebookCellType::Code;
        cell.outputs = {s, img, err};
        CHECK(NotebookCellOutputSlots(cell, 0.52) == 2 + 29 + 2);
        cell.type = NotebookCellType::Markdown;
        CHECK(NotebookCellOutputSlots(cell, 0.52) == 0);

        CHECK(NotebookContentKey("abc") == NotebookContentKey("abc"));
        CHECK(NotebookContentKey("abc") != NotebookContentKey("abd"));
        CHECK(NotebookContentKey("").size() == 16);
    }

    // --- Append: consecutive same-name streams merge, others don't
    {
        NotebookCell cell;
        NotebookOutput a;
        a.kind = NotebookOutput::Kind::Stream;
        a.name = "stdout";
        a.text = "1\n";
        NotebookOutput b = a;
        b.text = "2\n";
        NotebookOutput e = a;
        e.name = "stderr";
        e.text = "oops\n";
        NotebookAppendOutput(&cell, a);
        NotebookAppendOutput(&cell, b);
        NotebookAppendOutput(&cell, e);
        NotebookAppendOutput(&cell, b);
        CHECK(cell.outputs.size() == 3);
        CHECK(cell.outputs[0].text == "1\n2\n");
        CHECK(cell.outputs[1].name == "stderr");
        CHECK(cell.outputs[2].text == "2\n");

        NotebookCell flood;
        NotebookOutput chunk = a;
        chunk.text = std::string(kNotebookMaxStreamBytes / 2 + 10, 'x');
        NotebookAppendOutput(&flood, chunk);
        NotebookAppendOutput(&flood, chunk);
        NotebookAppendOutput(&flood, chunk);
        CHECK(flood.outputs.size() == 1);
        CHECK(flood.outputs[0].text.size() < kNotebookMaxStreamBytes + 64);
        CHECK(flood.outputs[0].text.find("[output truncated]") != std::string::npos);
    }

    // --- Serialize kernel-produced outputs (no raw JSON) in nbformat shape
    {
        NotebookDoc d;
        NotebookCell c;
        c.type = NotebookCellType::Code;
        c.id = "deadbeef";
        c.source = "print(1)\n1";
        c.execution_count = 7;
        c.metadata = Json::Object();
        NotebookOutput s;
        s.kind = NotebookOutput::Kind::Stream;
        s.name = "stdout";
        s.text = "1\n";
        NotebookOutput r;
        r.kind = NotebookOutput::Kind::ExecuteResult;
        r.text = "1";
        r.execution_count = 7;
        NotebookOutput img;
        img.kind = NotebookOutput::Kind::DisplayData;
        img.image_png = kTinyPng;
        NotebookOutput err;
        err.kind = NotebookOutput::Kind::Error;
        err.ename = "E";
        err.evalue = "v";
        err.text = "Traceback\nE: v";
        c.outputs = {s, r, img, err};
        d.cells.push_back(c);
        d.metadata = Json::Object();
        std::string out = SerializeNotebook(d);
        NotebookDoc back;
        CHECK(ParseNotebook(out, &back, nullptr));
        CHECK(back.cells.size() == 1);
        CHECK(back.cells[0].id == "deadbeef");
        CHECK(back.cells[0].execution_count == 7);
        CHECK(back.cells[0].source == "print(1)\n1");
        CHECK(back.cells[0].outputs.size() == 4);
        CHECK(back.cells[0].outputs[0].text == "1\n");
        CHECK(back.cells[0].outputs[1].kind == NotebookOutput::Kind::ExecuteResult);
        CHECK(back.cells[0].outputs[1].execution_count == 7);
        CHECK(back.cells[0].outputs[2].image_png == kTinyPng);
        CHECK(back.cells[0].outputs[2].image_width == 1);
        CHECK(back.cells[0].outputs[3].kind == NotebookOutput::Kind::Error);
        CHECK(back.cells[0].outputs[3].text == "Traceback\nE: v");
        CHECK(out.find("\"nbformat_minor\": 5") != std::string::npos);
        CHECK(out.find("\"image/png\": \"") != std::string::npos);
    }

    // --- Kernel protocol
    {
        std::string req = NotebookKernelExecuteRequest(3, "print(\"x\")\n");
        Json j;
        CHECK(Json::Parse(req, &j));
        CHECK(j.get("op").as_string() == "execute");
        CHECK(j.get("id").as_int() == 3);
        CHECK(j.get("code").as_string() == "print(\"x\")\n");
        CHECK(req.back() == '\n');
        CHECK(NotebookKernelShutdownRequest() == "{\"op\":\"shutdown\"}\n");

        NotebookKernelMessage m;
        CHECK(ParseNotebookKernelMessage("{\"type\":\"stream\",\"id\":2,\"name\":\"stdout\",\"text\":\"hi\\n\"}", &m));
        CHECK(m.type == "stream" && m.id == 2 && m.name == "stdout" && m.text == "hi\n");
        CHECK(ParseNotebookKernelMessage(
            "{\"type\":\"error\",\"id\":2,\"ename\":\"ValueError\",\"evalue\":\"boom\",\"traceback\":[\"a\",\"b\"]}", &m));
        CHECK(m.ename == "ValueError" && m.traceback.size() == 2 && m.traceback[1] == "b");
        CHECK(ParseNotebookKernelMessage("{\"type\":\"done\",\"id\":2,\"execution_count\":9}", &m));
        CHECK(m.execution_count == 9);
        CHECK(ParseNotebookKernelMessage("{\"type\":\"ready\",\"python\":\"3.12.0\"}", &m));
        CHECK(m.python == "3.12.0" && m.id == -1);
        CHECK(ParseNotebookKernelMessage("{\"type\":\"execute_result\",\"id\":1,\"text\":\"42\",\"png\":null}", &m));
        CHECK(m.png.empty() && m.text == "42");
        CHECK(!ParseNotebookKernelMessage("garbage", &m));
        CHECK(!ParseNotebookKernelMessage("{\"id\":1}", &m));

        std::string script = NotebookKernelScript();
        CHECK(script.find("\"type\": \"ready\"") != std::string::npos);
        CHECK(script.find("mep_nb_backend") != std::string::npos);
    }

    // --- Per-cell kernels
    {
        std::vector<NotebookKernelSpec> specs = {
            {"python3", "Python 3", "py", {}, NotebookKernelSpec::Mode::Python},
            {"ir", "R", "r", {"R", "--slave"}, NotebookKernelSpec::Mode::Script},
            {"javascript", "JavaScript", "js", {"node"}, NotebookKernelSpec::Mode::Script},
        };
        CHECK(FindNotebookKernel(specs, "ir") != nullptr);
        CHECK(FindNotebookKernel(specs, "ir")->language == "r");
        CHECK(FindNotebookKernel(specs, "nope") == nullptr);

        NotebookCell cell;
        CHECK(NotebookCellKernel(cell).empty());
        NotebookSetCellKernel(&cell, "ir");
        CHECK(NotebookCellKernel(cell) == "ir");
        CHECK(cell.metadata.get("kernel").as_string() == "ir");
        NotebookSetCellKernel(&cell, "");
        CHECK(cell.metadata.get("kernel").as_string() == "");

        // Default kernel: kernelspec.name wins when registered.
        NotebookDoc d;
        d.metadata = Json::Object();
        d.metadata["kernelspec"] = Json::Object();
        d.metadata["kernelspec"]["name"] = Json("ir");
        CHECK(NotebookDefaultKernel(d, specs) == "ir");
        // An unregistered kernelspec.name falls to a language match...
        d.metadata["kernelspec"]["name"] = Json("ir64");
        d.metadata["kernelspec"]["language"] = Json("javascript");
        CHECK(NotebookDefaultKernel(d, specs) == "javascript");
        // ...then to the first registered kernel.
        d.metadata = Json::Object();
        CHECK(NotebookDefaultKernel(d, specs) == "python3");
        // Empty registry: the built-in python3 name.
        CHECK(NotebookDefaultKernel(d, {}) == "python3");

        CHECK(NotebookKernelLanguageName(specs[0]) == "python");
        CHECK(NotebookKernelLanguageName(specs[1]) == "r");

        // Script exit folding.
        NotebookCell sc;
        NotebookAppendScriptExit(&sc, 0, "node");   // success: no output
        CHECK(sc.outputs.empty());
        NotebookAppendScriptExit(&sc, 3, "node");
        CHECK(sc.outputs.size() == 1 && sc.outputs[0].kind == NotebookOutput::Kind::Error);
        CHECK(sc.outputs[0].evalue.find("status 3") != std::string::npos);
        NotebookCell sc2;
        NotebookAppendScriptExit(&sc2, -1, "R");
        CHECK(sc2.outputs[0].evalue.find("could not start R") != std::string::npos);
    }

    // --- Kernel inheritance: a hand-typed new marker keeps the previous cell's kernel
    {
        NotebookDoc d;
        std::vector<std::string> lines = {"# %%", "a = 1"};
        std::vector<NotebookCellSpan> spans = ScanNotebookCells(lines);
        int uid = 1;
        SyncNotebookFromLines(&d, lines, spans, &uid);
        CHECK(d.cells.size() == 1);
        NotebookSetCellKernel(&d.cells[0], "ir");

        // Append a second cell in the text; it should inherit "ir".
        lines = {"# %%", "a = 1", "", "# %%", "b = 2"};
        spans = ScanNotebookCells(lines);
        SyncNotebookFromLines(&d, lines, spans, &uid);
        CHECK(d.cells.size() == 2);
        CHECK(NotebookCellKernel(d.cells[0]) == "ir");
        CHECK(NotebookCellKernel(d.cells[1]) == "ir");
    }

    std::printf("notebook_doc tests passed\n");
    return 0;
}

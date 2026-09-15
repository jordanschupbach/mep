#ifndef MEP_NOTEBOOK_DOC_H
#define MEP_NOTEBOOK_DOC_H

#include <string>
#include <vector>

#include "json.h"

// Jupyter notebook (.ipynb) support -- the raylib-free half, same
// reasoning as org_doc.h/sheet_doc.h: the notebook model, its JSON
// (nbformat 4) reader/writer, the text-view conversion, and the kernel
// wire protocol are all plain data + string code, usable/testable without
// a GL context. editor.cpp owns the per-buffer NotebookSession (kernel
// process, run queue, cell edits); main.cpp owns drawing cell chrome and
// output blocks under each code cell.
//
// Key architectural decision (mirrors org's Kanban/Gantt views, not the
// spreadsheet's separate Workbook): a notebook buffer's Buffer::lines IS
// editable text -- the notebook rendered in the "percent" cell format
// Jupytext/VS Code/Spyder all understand:
//
//     # %% [markdown]
//     Some *markdown* prose, kept raw (not `# `-commented the way
//     Jupytext writes it -- this text is never run as a script).
//
//     # %%
//     print("a code cell")
//
// so every ordinary vim motion/edit/undo/search/LSP feature works on cell
// sources with no notebook-specific input mode at all. Only what the text
// can't carry -- outputs, execution counts, cell ids, notebook/cell
// metadata -- lives in the side model (NotebookDoc), re-attached to the
// current text by cell index/source (SyncNotebookFromLines) and written
// back together with the text as real .ipynb JSON on `:w`.
//
// Scope, matching this repo's convention of naming exclusions:
//   - Outputs shown: text/plain (streams, results, tracebacks) and
//     image/png. Any other mime bundle (text/html, application/json,
//     widgets, ...) round-trips untouched through `raw` but displays as
//     its text/plain fallback, or a one-line placeholder without one.
//   - Markdown cells are shown as raw markdown source with markdown
//     syntax highlighting -- not rendered to rich text.
//   - One kernel: the local `python3` driven through NotebookKernelScript()
//     over stdin/stdout JSON lines. No jupyter_client/ZMQ, no other
//     languages, no stdin (input()) from a cell.
//   - Cell attachments, nbformat < 4, and per-cell "title" text on a
//     `# %%` marker are not modeled (a title typed after the marker is
//     ignored and dropped on save).

enum class NotebookCellType { Code, Markdown, Raw };

struct NotebookOutput {
    enum class Kind { Stream, ExecuteResult, DisplayData, Error };
    Kind kind = Kind::Stream;
    std::string name;   // Stream: "stdout" or "stderr"
    std::string text;   // text/plain: stream text, result repr, or an error's traceback joined by '\n'
    // image/png as base64 (whitespace stripped) for ExecuteResult/
    // DisplayData; empty when the bundle has no PNG. image_key is a
    // content hash of it (stable across frames, cheap to compare), what
    // main.cpp keys its decoded-texture cache by.
    std::string image_png;
    std::string image_key;
    int image_width = 0;   // from the PNG IHDR; 0 = unknown
    int image_height = 0;
    std::string ename, evalue;  // Error
    int execution_count = -1;   // ExecuteResult; -1 = null
    // The output's original JSON object when it was loaded from a file
    // rather than produced by this process's kernel -- written back
    // verbatim on save so mime types this viewer doesn't understand
    // survive a load/save round trip. Null for kernel-produced outputs.
    Json raw;
};

struct NotebookCell {
    NotebookCellType type = NotebookCellType::Code;
    std::string id;         // nbformat 4.5 cell id; generated when missing
    std::string source;     // lines joined by '\n', no trailing newline
    Json metadata;          // preserved verbatim (an object)
    std::vector<NotebookOutput> outputs;   // code cells only
    int execution_count = -1;              // -1 = null
    // Process-unique tag assigned by the editor session so an in-flight
    // kernel reply can find its cell after edits have shifted indices.
    int uid = 0;
    // Transient run state (never persisted).
    enum class RunState { Idle, Queued, Running };
    RunState run_state = RunState::Idle;
};

struct NotebookDoc {
    std::vector<NotebookCell> cells;
    Json metadata;   // notebook-level metadata (kernelspec, language_info, ...) preserved verbatim
    int nbformat = 4;
    int nbformat_minor = 5;
};

// --- Extension check ---------------------------------------------------
bool IsIpynbPath(const std::string &path);

// --- nbformat JSON <-> NotebookDoc ---------------------------------------
// Parse failure leaves *out untouched and puts a short reason in *error
// (when non-null). A notebook with no "cells" key is rejected; unknown
// cell types load as Raw.
bool ParseNotebook(const std::string &json_text, NotebookDoc *out, std::string *error);
// Pretty-printed (1-space indent, Jupyter's own style) nbformat 4 JSON.
// Cells/outputs produced here use nbformat's alphabetical key order;
// preserved `metadata`/`raw` objects keep their own.
std::string SerializeNotebook(const NotebookDoc &doc);
// Generates a fresh 8-hex-digit cell id.
std::string NotebookNewCellId();

// --- Text view (percent format) ------------------------------------------
std::string NotebookCellMarker(NotebookCellType type);   // "# %%", "# %% [markdown]", "# %% [raw]"
// True if `line` is a cell marker; *type receives the cell type it opens.
bool ParseNotebookMarker(const std::string &line, NotebookCellType *type);
// Marker + source lines per cell, one blank line between cells.
std::vector<std::string> NotebookToLines(const NotebookDoc &doc);

struct NotebookCellSpan {
    int marker_row = -1;  // -1: implicit leading cell (content before the first marker)
    int first_row = 0;    // first body row
    int end_row = 0;      // one past the last body row (== first_row for an empty body)
    NotebookCellType type = NotebookCellType::Code;
};
// Every cell in `lines`, in order. A leading run of lines before the
// first marker is an implicit Code cell only if it has non-blank content;
// otherwise it's skipped (so a blank first line doesn't create a cell).
std::vector<NotebookCellSpan> ScanNotebookCells(const std::vector<std::string> &lines);
// The span's body joined by '\n' with trailing blank lines dropped (the
// blank separator line NotebookToLines emits between cells isn't source).
std::string NotebookSpanSource(const std::vector<std::string> &lines, const NotebookCellSpan &span);
// Index of the span containing `row` (marker or body row), or -1.
int NotebookSpanAtRow(const std::vector<NotebookCellSpan> &spans, int row);

// Re-attaches doc->cells to the text's current cells. Same cell count:
// each cell just takes its span's source/type in place (an in-place edit
// keeps its outputs, like editing a cell in Jupyter does). Different
// count (a marker was added/removed): cells are re-matched to spans by
// exact source+type in document order; unmatched spans become fresh
// cells (new id, uid from *next_uid), unmatched old cells are dropped.
// A cell whose type changed away from Code loses its outputs.
void SyncNotebookFromLines(NotebookDoc *doc, const std::vector<std::string> &lines,
                           const std::vector<NotebookCellSpan> &spans, int *next_uid);

// --- Output display ------------------------------------------------------
// How many text lines of one output block are shown before it's cut off
// with a "... (N more lines)" line, and how many buffer-line-heights an
// image output claims (derived from its aspect ratio, clamped).
constexpr int kNotebookMaxOutputLines = 40;
constexpr int kNotebookImageWidthChars = 72;
constexpr int kNotebookImageMinSlots = 3;
constexpr int kNotebookImageMaxSlots = 32;
// Total bytes of stream text kept per cell before further output is
// dropped with a trailing "[output truncated]" marker.
constexpr size_t kNotebookMaxStreamBytes = 256 * 1024;

std::string StripAnsi(const std::string &s);
// The lines an output renders as (ANSI stripped, capped at
// kNotebookMaxOutputLines + one truncation line). Empty for an image-only
// output (the texture stands in for text).
std::vector<std::string> NotebookOutputDisplayLines(const NotebookOutput &out);
// Visual slots (line-heights) one output claims: its display lines, or
// its image's slot count. `char_aspect` is the renderer's current
// char-width / line-height ratio (an image kNotebookImageWidthChars wide
// is that many chars * char_aspect line-heights tall per unit of its own
// aspect) -- passed in rather than assumed so the reserved height matches
// what's actually drawn at any font size. Must be a pure function of the
// output data + that one number so every slot-counting site (DrawPane,
// its cursor lookup, Editor::UpdateScrollForPane) agrees exactly.
int NotebookOutputSlots(const NotebookOutput &out, double char_aspect);
int NotebookImageSlots(int width, int height, double char_aspect);
// Sum over the cell's outputs -- the trailing block's height, 0 if none.
int NotebookCellOutputSlots(const NotebookCell &cell, double char_aspect);
// The ratio assumed when no renderer has reported one yet (the built-in
// font at its default size).
constexpr double kNotebookDefaultCharAspect = 0.52;
// Reads width/height out of a base64 PNG's IHDR without a full decode.
bool PngDimensionsFromBase64(const std::string &b64, int *width, int *height);
// Cheap content hash (FNV-1a, hex) for NotebookOutput::image_key.
std::string NotebookContentKey(const std::string &data);
// Appends one output, merging consecutive same-name streams into one
// (as Jupyter's own frontend does) and enforcing kNotebookMaxStreamBytes.
void NotebookAppendOutput(NotebookCell *cell, NotebookOutput out);

// --- Kernel wire protocol ------------------------------------------------
// mep -> kernel (one JSON object per line on the kernel's stdin):
//   {"op":"execute","id":N,"code":"..."}   {"op":"shutdown"}
// kernel -> mep (one JSON object per line on its stdout):
//   {"type":"ready","python":"3.12.1"}
//   {"type":"stream","id":N,"name":"stdout"|"stderr","text":"..."}
//   {"type":"execute_result","id":N,"text":"repr","png":"<b64>"|null}
//   {"type":"display_data","id":N,"png":"<b64>","text":"..."|null}
//   {"type":"error","id":N,"ename":"...","evalue":"...","traceback":[...]}
//   {"type":"done","id":N,"execution_count":K}
struct NotebookKernelMessage {
    std::string type;
    int id = -1;
    std::string name, text, png;
    std::string ename, evalue;
    std::vector<std::string> traceback;
    int execution_count = -1;
    std::string python;  // "ready"
};
bool ParseNotebookKernelMessage(const std::string &line, NotebookKernelMessage *out);
std::string NotebookKernelExecuteRequest(int id, const std::string &code);
std::string NotebookKernelShutdownRequest();
// The Python program run as `python3 -u -c <script>`: a persistent
// namespace, IPython-style last-expression display, per-cell traceback
// trimming, and an inline matplotlib backend that ships figures back as
// PNG display_data at the end of each cell (or on plt.show()).
const char *NotebookKernelScript();

#endif  // MEP_NOTEBOOK_DOC_H

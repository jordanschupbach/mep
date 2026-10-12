#include "notebook_doc.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string_view>

namespace {

std::string LowerExt(const std::string &path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string ext = path.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// nbformat "multiline string": either one string or a list of line
// strings (each usually ending in "\n" except the last). Both forms load
// to the same joined text.
std::string JoinMultiline(const Json &v) {
    if (v.is_string()) return v.as_string();
    std::string out;
    if (v.is_array()) {
        for (const Json &item : v.items()) {
            if (item.is_string()) out += item.as_string();
        }
    }
    return out;
}

std::string StripTrailingNewlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// The inverse: text -> list of lines, every line but the last keeping its
// "\n" (nbformat's own convention, so a round trip through Jupyter is a
// no-op diff). Empty text -> empty list.
Json SplitMultiline(const std::string &text) {
    Json arr = Json::Array();
    if (text.empty()) return arr;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            arr.push_back(Json(text.substr(start)));
            break;
        }
        arr.push_back(Json(text.substr(start, nl - start + 1)));
        start = nl + 1;
    }
    return arr;
}

// Base64 with whitespace (Jupyter writes PNG data as "....\n", and some
// tools wrap it at 76 columns) stripped, so the key/dimension helpers see
// one clean payload.
std::string StripWhitespace(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) out += c;
    }
    return out;
}

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// Decodes at most `max_bytes` bytes from the front of a base64 payload.
std::vector<unsigned char> Base64Prefix(const std::string &b64, size_t max_bytes) {
    std::vector<unsigned char> out;
    int acc = 0, bits = 0;
    for (char c : b64) {
        if (c == '=') break;
        int v = Base64Value(c);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
            if (out.size() >= max_bytes) break;
        }
    }
    return out;
}

// One-space-indented pretty printer matching `json.dump(nb, indent=1)`,
// what Jupyter itself writes -- so a notebook saved here diffs cleanly
// against one saved by Jupyter. Json::dump() is compact-only.
void PrettyDump(const Json &v, int depth, std::string &out) {
    std::string pad(static_cast<size_t>(depth + 1), ' ');
    std::string close_pad(static_cast<size_t>(depth), ' ');
    if (v.is_array()) {
        if (v.size() == 0) {
            out += "[]";
            return;
        }
        out += "[\n";
        const std::vector<Json> &items = v.items();
        for (size_t i = 0; i < items.size(); i++) {
            out += pad;
            PrettyDump(items[i], depth + 1, out);
            out += (i + 1 < items.size()) ? ",\n" : "\n";
        }
        out += close_pad + "]";
        return;
    }
    if (v.is_object()) {
        if (v.size() == 0) {
            out += "{}";
            return;
        }
        out += "{\n";
        const auto &fields = v.fields();
        for (size_t i = 0; i < fields.size(); i++) {
            out += pad;
            out += Json(fields[i].first).dump();
            out += ": ";
            PrettyDump(fields[i].second, depth + 1, out);
            out += (i + 1 < fields.size()) ? ",\n" : "\n";
        }
        out += close_pad + "}";
        return;
    }
    out += v.dump();
}

NotebookOutput ParseOutput(const Json &o) {
    NotebookOutput out;
    out.raw = o;
    std::string type = o.get("output_type").as_string("");
    if (type == "stream") {
        out.kind = NotebookOutput::Kind::Stream;
        out.name = o.get("name").as_string("stdout");
        out.text = JoinMultiline(o.get("text"));
    } else if (type == "error") {
        out.kind = NotebookOutput::Kind::Error;
        out.ename = o.get("ename").as_string("");
        out.evalue = o.get("evalue").as_string("");
        std::string joined;
        for (const Json &line : o.get("traceback").items()) {
            if (!line.is_string()) continue;
            if (!joined.empty()) joined += '\n';
            joined += line.as_string();
        }
        out.text = joined;
    } else {
        out.kind = (type == "execute_result") ? NotebookOutput::Kind::ExecuteResult : NotebookOutput::Kind::DisplayData;
        const Json &ec = o.get("execution_count");
        if (ec.is_number()) out.execution_count = ec.as_int();
        const Json &data = o.get("data");
        if (data.contains("text/plain")) {
            out.text = StripTrailingNewlines(JoinMultiline(data.get("text/plain")));
        } else {
            // No text fallback: name the richest mime type we can't show so
            // the block isn't silently empty.
            for (const auto &kv : data.fields()) {
                if (kv.first != "image/png") {
                    out.text = "<" + kv.first + " output>";
                    break;
                }
            }
        }
        if (data.contains("image/png")) {
            out.image_png = StripWhitespace(JoinMultiline(data.get("image/png")));
            out.image_key = NotebookContentKey(out.image_png);
            PngDimensionsFromBase64(out.image_png, &out.image_width, &out.image_height);
        }
    }
    return out;
}

Json OutputToJson(const NotebookOutput &out) {
    if (!out.raw.is_null()) return out.raw;
    Json o = Json::Object();
    switch (out.kind) {
        case NotebookOutput::Kind::Stream:
            o["name"] = Json(out.name);
            o["output_type"] = Json("stream");
            o["text"] = SplitMultiline(out.text);
            break;
        case NotebookOutput::Kind::Error: {
            o["ename"] = Json(out.ename);
            o["evalue"] = Json(out.evalue);
            o["output_type"] = Json("error");
            Json tb = Json::Array();
            if (!out.text.empty()) {
                size_t start = 0;
                while (true) {
                    size_t nl = out.text.find('\n', start);
                    tb.push_back(Json(out.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start)));
                    if (nl == std::string::npos) break;
                    start = nl + 1;
                }
            }
            o["traceback"] = tb;
            break;
        }
        case NotebookOutput::Kind::ExecuteResult:
        case NotebookOutput::Kind::DisplayData: {
            Json data = Json::Object();
            if (!out.image_png.empty()) data["image/png"] = Json(out.image_png + "\n");
            if (!out.text.empty() || out.image_png.empty()) data["text/plain"] = SplitMultiline(out.text);
            o["data"] = data;
            if (out.kind == NotebookOutput::Kind::ExecuteResult) {
                o["execution_count"] = out.execution_count >= 0 ? Json(out.execution_count) : Json(nullptr);
            }
            o["metadata"] = Json::Object();
            o["output_type"] = Json(out.kind == NotebookOutput::Kind::ExecuteResult ? "execute_result" : "display_data");
            break;
        }
    }
    return o;
}

}  // namespace

bool IsIpynbPath(const std::string &path) { return LowerExt(path) == "ipynb"; }

std::string NotebookNewCellId() {
    static std::mt19937 rng{std::random_device{}()};
    static const char *const kHex = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 8; i++) id += kHex[rng() & 15u];
    return id;
}

bool ParseNotebook(const std::string &json_text, NotebookDoc *out, std::string *error) {
    Json root;
    if (!Json::Parse(json_text, &root) || !root.is_object()) {
        if (error) *error = "not valid JSON";
        return false;
    }
    if (!root.get("cells").is_array()) {
        if (error) *error = "no \"cells\" array (not an nbformat 4 notebook)";
        return false;
    }
    NotebookDoc doc;
    doc.nbformat = root.get("nbformat").as_int(4);
    doc.nbformat_minor = root.get("nbformat_minor").as_int(5);
    doc.metadata = root.get("metadata").is_object() ? root.get("metadata") : Json::Object();
    for (const Json &c : root.get("cells").items()) {
        if (!c.is_object()) continue;
        NotebookCell cell;
        std::string type = c.get("cell_type").as_string("code");
        cell.type = type == "code" ? NotebookCellType::Code
                    : type == "markdown" ? NotebookCellType::Markdown
                                         : NotebookCellType::Raw;
        cell.id = c.get("id").as_string("");
        if (cell.id.empty()) cell.id = NotebookNewCellId();
        cell.source = StripTrailingNewlines(JoinMultiline(c.get("source")));
        cell.metadata = c.get("metadata").is_object() ? c.get("metadata") : Json::Object();
        if (cell.type == NotebookCellType::Code) {
            const Json &ec = c.get("execution_count");
            if (ec.is_number()) cell.execution_count = ec.as_int();
            for (const Json &o : c.get("outputs").items()) {
                if (o.is_object()) cell.outputs.push_back(ParseOutput(o));
            }
        }
        doc.cells.push_back(std::move(cell));
    }
    *out = std::move(doc);
    return true;
}

std::string SerializeNotebook(const NotebookDoc &doc) {
    Json root = Json::Object();
    Json cells = Json::Array();
    for (const NotebookCell &cell : doc.cells) {
        Json c = Json::Object();
        c["cell_type"] = Json(cell.type == NotebookCellType::Code       ? "code"
                              : cell.type == NotebookCellType::Markdown ? "markdown"
                                                                        : "raw");
        if (cell.type == NotebookCellType::Code) {
            c["execution_count"] = cell.execution_count >= 0 ? Json(cell.execution_count) : Json(nullptr);
        }
        c["id"] = Json(cell.id.empty() ? NotebookNewCellId() : cell.id);
        c["metadata"] = cell.metadata.is_object() ? cell.metadata : Json::Object();
        if (cell.type == NotebookCellType::Code) {
            Json outputs = Json::Array();
            for (const NotebookOutput &o : cell.outputs) outputs.push_back(OutputToJson(o));
            c["outputs"] = outputs;
        }
        c["source"] = SplitMultiline(cell.source);
        cells.push_back(std::move(c));
    }
    root["cells"] = cells;
    root["metadata"] = doc.metadata.is_object() ? doc.metadata : Json::Object();
    root["nbformat"] = Json(doc.nbformat);
    root["nbformat_minor"] = Json(doc.nbformat_minor);
    std::string out;
    PrettyDump(root, 0, out);
    out += "\n";
    return out;
}

// --- Text view -----------------------------------------------------------

std::string NotebookCellMarker(NotebookCellType type) {
    switch (type) {
        case NotebookCellType::Markdown: return "# %% [markdown]";
        case NotebookCellType::Raw: return "# %% [raw]";
        case NotebookCellType::Code: break;
    }
    return "# %%";
}

bool ParseNotebookMarker(const std::string &line, NotebookCellType *type) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    if (line.compare(i, 4, "# %%") != 0) return false;
    size_t after = i + 4;
    if (after < line.size() && !std::isspace(static_cast<unsigned char>(line[after]))) return false;
    std::string rest = line.substr(after);
    NotebookCellType t = NotebookCellType::Code;
    if (rest.find("[markdown]") != std::string::npos || rest.find("[md]") != std::string::npos) {
        t = NotebookCellType::Markdown;
    } else if (rest.find("[raw]") != std::string::npos) {
        t = NotebookCellType::Raw;
    }
    if (type) *type = t;
    return true;
}

std::vector<std::string> NotebookToLines(const NotebookDoc &doc) {
    std::vector<std::string> lines;
    for (size_t i = 0; i < doc.cells.size(); i++) {
        const NotebookCell &cell = doc.cells[i];
        if (i > 0) lines.emplace_back("");
        lines.push_back(NotebookCellMarker(cell.type));
        size_t start = 0;
        if (!cell.source.empty()) {
            while (true) {
                size_t nl = cell.source.find('\n', start);
                lines.push_back(cell.source.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
    }
    if (lines.empty()) lines.push_back(NotebookCellMarker(NotebookCellType::Code));
    return lines;
}

std::vector<NotebookCellSpan> ScanNotebookCells(const std::vector<std::string> &lines) {
    std::vector<NotebookCellSpan> spans;
    int n = static_cast<int>(lines.size());
    int first_marker = -1;
    for (int r = 0; r < n; r++) {
        if (ParseNotebookMarker(lines[static_cast<size_t>(r)], nullptr)) {
            first_marker = r;
            break;
        }
    }
    int lead_end = first_marker < 0 ? n : first_marker;
    bool lead_has_content = false;
    for (int r = 0; r < lead_end; r++) {
        const std::string &l = lines[static_cast<size_t>(r)];
        if (l.find_first_not_of(" \t\r") != std::string::npos) {
            lead_has_content = true;
            break;
        }
    }
    if (lead_has_content) {
        NotebookCellSpan s;
        s.marker_row = -1;
        s.first_row = 0;
        s.end_row = lead_end;
        s.type = NotebookCellType::Code;
        spans.push_back(s);
    }
    if (first_marker < 0) return spans;
    int r = first_marker;
    while (r < n) {
        NotebookCellSpan s;
        s.marker_row = r;
        ParseNotebookMarker(lines[static_cast<size_t>(r)], &s.type);
        s.first_row = r + 1;
        int e = r + 1;
        while (e < n && !ParseNotebookMarker(lines[static_cast<size_t>(e)], nullptr)) e++;
        s.end_row = e;
        spans.push_back(s);
        r = e;
    }
    return spans;
}

std::string NotebookSpanSource(const std::vector<std::string> &lines, const NotebookCellSpan &span) {
    // An indented or blockquoted chunk's code loses its opener's prefix,
    // as knitr's parse_block strips it (or the prefix without its trailing
    // spaces, for a bare `>` line).
    std::string bare = span.indent;
    while (!bare.empty() && (bare.back() == ' ' || bare.back() == '\t')) bare.pop_back();
    auto body = [&](int r) -> std::string_view {
        std::string_view l = lines[static_cast<size_t>(r)];
        if (!span.indent.empty() && l.starts_with(span.indent)) return l.substr(span.indent.size());
        if (!bare.empty() && l.starts_with(bare)) return l.substr(bare.size());
        return l;
    };
    int end = span.end_row;
    while (end > span.first_row && body(end - 1).find_first_not_of(" \t\r") == std::string_view::npos) end--;
    std::string out;
    for (int r = span.first_row; r < end; r++) {
        if (r > span.first_row) out += '\n';
        out += body(r);
    }
    return out;
}

int NotebookSpanAtRow(const std::vector<NotebookCellSpan> &spans, int row) {
    for (size_t i = 0; i < spans.size(); i++) {
        int start = spans[i].marker_row >= 0 ? spans[i].marker_row : spans[i].first_row;
        int end = std::max(spans[i].end_row, start + 1);
        if (spans[i].close_row >= 0) end = std::max(end, spans[i].close_row + 1);
        if (row >= start && row < end) return static_cast<int>(i);
    }
    return -1;
}

int NotebookSpanLastRow(const NotebookCellSpan &span) {
    if (span.close_row >= 0) return span.close_row;
    if (span.end_row > span.first_row) return span.end_row - 1;
    return span.marker_row >= 0 ? span.marker_row : span.first_row;
}

void SyncNotebookFromLines(NotebookDoc *doc, const std::vector<std::string> &lines,
                           const std::vector<NotebookCellSpan> &spans, int *next_uid) {
    auto fresh_cell = [&](const NotebookCellSpan &span, std::string source, const NotebookCell *above) {
        NotebookCell c;
        c.type = span.type;
        c.id = NotebookNewCellId();
        c.source = std::move(source);
        c.metadata = Json::Object();
        c.uid = (*next_uid)++;
        // A new block runs on the previous block's kernel (see the header
        // comment); a cell with no explicit kernel passes that on too.
        if (above) {
            std::string kernel = NotebookCellKernel(*above);
            if (!kernel.empty()) NotebookSetCellKernel(&c, kernel);
        }
        return c;
    };
    auto apply = [](NotebookCell &c, const NotebookCellSpan &span, std::string source) {
        c.source = std::move(source);
        if (c.type != span.type) {
            c.type = span.type;
            if (span.type != NotebookCellType::Code) {
                c.outputs.clear();
                c.execution_count = -1;
            }
        }
    };
    if (spans.size() == doc->cells.size()) {
        for (size_t i = 0; i < spans.size(); i++) {
            apply(doc->cells[i], spans[i], NotebookSpanSource(lines, spans[i]));
            if (doc->cells[i].uid == 0) doc->cells[i].uid = (*next_uid)++;
        }
        return;
    }
    std::vector<NotebookCell> old = std::move(doc->cells);
    std::vector<bool> used(old.size(), false);
    std::vector<NotebookCell> fresh;
    size_t search_from = 0;
    for (const NotebookCellSpan &span : spans) {
        std::string source = NotebookSpanSource(lines, span);
        bool matched = false;
        for (size_t i = search_from; i < old.size(); i++) {
            if (used[i] || old[i].type != span.type || old[i].source != source) continue;
            used[i] = true;
            search_from = i + 1;
            fresh.push_back(std::move(old[i]));
            if (fresh.back().uid == 0) fresh.back().uid = (*next_uid)++;
            matched = true;
            break;
        }
        if (!matched) fresh.push_back(fresh_cell(span, std::move(source), fresh.empty() ? nullptr : &fresh.back()));
    }
    doc->cells = std::move(fresh);
}

// --- R Markdown (.Rmd) ---------------------------------------------------

bool IsRmdPath(const std::string &path) { return LowerExt(path) == "rmd"; }

namespace {

bool BlankFrom(const std::string &line, size_t from) {
    return from >= line.size() || line.find_first_not_of(" \t\r", from) == std::string::npos;
}

std::string TrimAscii(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string Unquote(const std::string &s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) return s.substr(1, s.size() - 2);
    return s;
}

// knitr option lists are R argument lists: split on commas outside
// (), [], {} and quotes, so `fig.dim=c(5, 3)` stays one option.
std::vector<std::string> SplitChunkOptions(const std::string &s) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (quote) {
            cur += c;
            if (c == '\\' && i + 1 < s.size()) {
                cur += s[++i];
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
            depth--;
        } else if (c == ',' && depth == 0) {
            parts.push_back(TrimAscii(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    parts.push_back(TrimAscii(cur));
    return parts;
}

// knitr's chunk prefix: the indentation, tabs or blockquote markers
// (`[\t >]*`) before a fence's backticks, and where they end.
size_t RmdFencePrefixEnd(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '>')) i++;
    return i;
}

// knitr's chunk.end: `[\t >]*` then three or more backticks and nothing
// else. `*opener` receives the prefix and backticks (the part a matching
// close must repeat exactly).
bool IsRmdFenceLine(const std::string &line, std::string *opener) {
    size_t i = RmdFencePrefixEnd(line);
    size_t j = i;
    while (j < line.size() && line[j] == '`') j++;
    if (j - i < 3 || !BlankFrom(line, j)) return false;
    if (opener) *opener = line.substr(0, j);
    return true;
}

}  // namespace

bool ParseRmdChunkHeader(const std::string &line, RmdChunkHeader *out, int *fence_len) {
    // knitr's chunk.begin, `^[\t >]*```+\s*\{([a-zA-Z0-9_]+( *[ ,].*)?)\}\s*$`.
    const size_t prefix_end = RmdFencePrefixEnd(line);
    size_t after = prefix_end;
    while (after < line.size() && line[after] == '`') after++;
    const int run = static_cast<int>(after - prefix_end);
    if (run < 3) return false;
    size_t i = after;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= line.size() || line[i] != '{') return false;
    size_t close = line.find_last_of('}');
    if (close == std::string::npos || close < i || !BlankFrom(line, close + 1)) return false;
    std::string inner = line.substr(i + 1, close - i - 1);
    size_t e = 0;
    while (e < inner.size() && (std::isalnum(static_cast<unsigned char>(inner[e])) || inner[e] == '_')) e++;
    if (e == 0) return false;
    // The engine ends the braces or is followed by a space or comma:
    // `{r.x}` or `{r-1}` are not chunks to knitr.
    if (e < inner.size() && inner[e] != ' ' && inner[e] != ',') return false;
    RmdChunkHeader h;
    h.engine = inner.substr(0, e);
    h.indent = line.substr(0, prefix_end);
    h.fence = line.substr(0, after);
    std::string rest = inner.substr(e);
    // `{r label, ...}` and `{r, label, ...}` both name the chunk: drop
    // one leading comma so the label (if any) is the first option.
    rest = TrimAscii(rest);
    if (!rest.empty() && rest[0] == ',') rest = TrimAscii(rest.substr(1));
    if (!rest.empty()) {
        std::vector<std::string> parts = SplitChunkOptions(rest);
        for (size_t k = 0; k < parts.size(); k++) {
            const std::string &part = parts[k];
            if (part.empty()) continue;
            size_t eq = part.find('=');
            if (eq == std::string::npos) {
                if (k == 0) h.label = Unquote(part);
                continue;
            }
            std::string key = TrimAscii(part.substr(0, eq));
            std::string value = TrimAscii(part.substr(eq + 1));
            if (key == "label") h.label = Unquote(value);
            if (key == "eval" && (value == "FALSE" || value == "F")) h.eval = false;
            h.options.emplace_back(key, value);
        }
    }
    if (out) *out = std::move(h);
    if (fence_len) *fence_len = run;
    return true;
}

// Which lines are chunks is decided exactly as knitr's own splitter
// (knitr:::group_indices, 1.51) decides it, so what mep runs is what a knit
// runs -- including where that surprises: knitr knows nothing of plain
// fences, so a ```{r} quoted inside a ```` or ~~~ block *is* a chunk (the
// knitr way to show one literally is `r ''` before it). A chunk ends at a
// fence with exactly its opener's prefix and backticks; another fence
// line is part of the code if such a close follows (with no new chunk
// before it). A new opener with the same prefix and backticks inside a
// chunk starts a new chunk, leaving the first unclosed. Where knitr stops
// with "does not match the opening fence", any fence line closes the chunk.
std::vector<NotebookCellSpan> ScanRmdChunks(const std::vector<std::string> &lines) {
    std::vector<NotebookCellSpan> spans;
    const int n = static_cast<int>(lines.size());
    int prose_start = 0;
    auto flush_prose = [&](int end) {
        bool content = false;
        for (int r = prose_start; r < end && !content; r++) content = !BlankFrom(lines[static_cast<size_t>(r)], 0);
        if (!content) return;
        NotebookCellSpan s;
        s.marker_row = -1;
        s.first_row = prose_start;
        s.end_row = end;
        s.type = NotebookCellType::Markdown;
        spans.push_back(s);
    };
    // A chunk opener whose prefix + backticks are exactly `fence`
    // (knitr's match_chunk_begin); `more_ticks` also allows longer runs.
    auto opens_with = [&](int row, const std::string &fence, bool more_ticks) {
        RmdChunkHeader h;
        if (!ParseRmdChunkHeader(lines[static_cast<size_t>(row)], &h, nullptr)) return false;
        if (more_ticks) return h.fence.compare(0, fence.size(), fence) == 0 &&
                               h.fence.find_first_not_of('`', fence.size()) == std::string::npos;
        return h.fence == fence;
    };
    auto exact_close = [&](int row, const std::string &fence) {
        std::string f;
        return IsRmdFenceLine(lines[static_cast<size_t>(row)], &f) && f == fence;
    };
    int r = 0;
    while (r < n) {
        RmdChunkHeader h;
        if (!ParseRmdChunkHeader(lines[static_cast<size_t>(r)], &h, nullptr)) {
            r++;
            continue;
        }
        flush_prose(r);
        NotebookCellSpan s;
        s.marker_row = r;
        s.first_row = r + 1;
        s.type = NotebookCellType::Code;
        s.engine = h.engine;
        s.label = h.label;
        s.eval = h.eval;
        s.indent = h.indent;
        int e = r + 1;
        int next = n;    // where scanning resumes
        bool closed = false;
        for (; e < n; e++) {
            if (opens_with(e, h.fence, false)) {
                next = e;   // a new chunk; this one stays unclosed
                break;
            }
            if (!IsRmdFenceLine(lines[static_cast<size_t>(e)], nullptr)) continue;
            if (exact_close(e, h.fence)) {
                closed = true;
                break;
            }
            int k = e + 1;
            while (k < n && !exact_close(k, h.fence)) k++;
            if (k < n) {
                bool begin_between = false;
                for (int b = e + 1; b < k && !begin_between; b++) begin_between = opens_with(b, h.fence, true);
                if (k == e + 1 || !begin_between) continue;   // just a line of code
            }
            closed = true;   // knitr would stop here; take it as the close
            break;
        }
        if (closed) {
            s.end_row = e;
            s.close_row = e;
            next = e + 1;
        } else {
            s.end_row = next;
            s.close_row = std::max(r, next - 1);
        }
        spans.push_back(s);
        r = next;
        prose_start = r;
    }
    flush_prose(n);
    return spans;
}

std::vector<std::string> MaskRmdForMath(const std::vector<std::string> &lines) {
    std::vector<std::string> out = lines;
    const int n = static_cast<int>(lines.size());
    auto blank_row = [&](int r) { std::fill(out[static_cast<size_t>(r)].begin(), out[static_cast<size_t>(r)].end(), ' '); };
    std::vector<bool> masked(lines.size(), false);
    auto mask_rows = [&](int first, int last) {
        for (int r = std::max(0, first); r <= last && r < n; r++) {
            blank_row(r);
            masked[static_cast<size_t>(r)] = true;
        }
    };
    // YAML front matter.
    if (n > 0 && TrimAscii(lines[0]) == "---") {
        int e = 1;
        while (e < n && TrimAscii(lines[static_cast<size_t>(e)]) != "---" && TrimAscii(lines[static_cast<size_t>(e)]) != "...") e++;
        if (e < n) mask_rows(0, e);
    }
    // Chunks, fences included.
    for (const NotebookCellSpan &sp : ScanRmdChunks(lines)) {
        if (sp.type == NotebookCellType::Code) mask_rows(sp.marker_row, std::max(sp.close_row, sp.end_row - 1));
    }
    // Plain ``` / ~~~ code shown in the prose.
    for (int r = 0; r < n; r++) {
        if (masked[static_cast<size_t>(r)]) continue;
        const std::string &l = lines[static_cast<size_t>(r)];
        size_t i = l.find_first_not_of(" \t>");
        if (i == std::string::npos || (l[i] != '`' && l[i] != '~')) continue;
        const char fence = l[i];
        size_t j = i;
        while (j < l.size() && l[j] == fence) j++;
        if (j - i < 3) continue;
        int e = r + 1;
        for (; e < n; e++) {
            const std::string &c = lines[static_cast<size_t>(e)];
            size_t a = c.find_first_not_of(" \t>");
            size_t b = a;
            while (b != std::string::npos && b < c.size() && c[b] == fence) b++;
            if (a != std::string::npos && b - a >= j - i && BlankFrom(c, b)) break;
        }
        mask_rows(r, e);
        r = e;
    }
    // Inline code: a run of N backticks to the next run of exactly N
    // (`r expr` included -- R, not math, whatever its $s).
    for (int r = 0; r < n; r++) {
        if (masked[static_cast<size_t>(r)]) continue;
        std::string &l = out[static_cast<size_t>(r)];
        size_t i = 0;
        while (i < l.size()) {
            if (l[i] != '`') {
                i++;
                continue;
            }
            size_t j = i;
            while (j < l.size() && l[j] == '`') j++;
            const size_t run = j - i;
            size_t k = j;
            size_t close = std::string::npos;
            while (k < l.size()) {
                if (l[k] != '`') {
                    k++;
                    continue;
                }
                size_t m = k;
                while (m < l.size() && l[m] == '`') m++;
                if (m - k == run) {
                    close = m;
                    break;
                }
                k = m;
            }
            if (close == std::string::npos) {
                i = j;   // an unmatched run is literal backticks
                continue;
            }
            std::fill(l.begin() + static_cast<std::ptrdiff_t>(i), l.begin() + static_cast<std::ptrdiff_t>(close), ' ');
            i = close;
        }
    }
    return out;
}

std::string SerializeNotebookOutputCache(const NotebookDoc &doc, const std::vector<NotebookCellSpan> &spans) {
    Json root = Json::Object();
    root["version"] = Json(1);
    Json cells = Json::Array();
    for (size_t i = 0; i < doc.cells.size(); i++) {
        const NotebookCell &cell = doc.cells[i];
        if (cell.type != NotebookCellType::Code) continue;
        if (cell.outputs.empty() && cell.execution_count < 0) continue;
        Json c = Json::Object();
        c["hash"] = Json(NotebookContentKey(cell.source));
        c["label"] = Json(i < spans.size() ? spans[i].label : std::string());
        c["execution_count"] = cell.execution_count >= 0 ? Json(cell.execution_count) : Json(nullptr);
        Json outputs = Json::Array();
        for (const NotebookOutput &o : cell.outputs) outputs.push_back(OutputToJson(o));
        c["outputs"] = outputs;
        cells.push_back(std::move(c));
    }
    root["cells"] = cells;
    return root.dump();
}

int ApplyNotebookOutputCache(NotebookDoc *doc, const std::string &json_text) {
    Json root;
    if (json_text.empty() || !Json::Parse(json_text, &root) || !root.is_object()) return 0;
    if (root.get("version").as_int(0) != 1) return 0;
    const std::vector<Json> &entries = root.get("cells").items();
    std::vector<bool> used(entries.size(), false);
    auto take = [&](NotebookCell &cell, size_t k) {
        used[k] = true;
        const Json &e = entries[k];
        cell.outputs.clear();
        for (const Json &o : e.get("outputs").items()) {
            if (o.is_object()) cell.outputs.push_back(ParseOutput(o));
        }
        const Json &ec = e.get("execution_count");
        cell.execution_count = ec.is_number() ? ec.as_int() : -1;
    };
    int restored = 0;
    for (size_t i = 0; i < doc->cells.size(); i++) {
        NotebookCell &cell = doc->cells[i];
        if (cell.type != NotebookCellType::Code) continue;
        const std::string hash = NotebookContentKey(cell.source);
        size_t pick = entries.size();
        for (size_t k = 0; k < entries.size() && pick == entries.size(); k++) {
            if (!used[k] && entries[k].get("hash").as_string("") == hash) pick = k;
        }
        if (pick == entries.size()) continue;
        take(cell, pick);
        restored++;
    }
    return restored;
}

// --- Per-cell kernels ----------------------------------------------------

std::string NotebookCellKernel(const NotebookCell &cell) { return cell.metadata.get(kNotebookCellKernelKey).as_string(""); }

void NotebookSetCellKernel(NotebookCell *cell, const std::string &name) {
    if (!cell->metadata.is_object()) cell->metadata = Json::Object();
    cell->metadata[kNotebookCellKernelKey] = Json(name);
}

const NotebookKernelSpec *FindNotebookKernel(const std::vector<NotebookKernelSpec> &specs, const std::string &name) {
    for (const NotebookKernelSpec &s : specs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::string NotebookKernelLanguageName(const NotebookKernelSpec &spec) {
    if (spec.language == "py" || spec.language == "python") return "python";
    if (spec.language == "js" || spec.language == "javascript") return "javascript";
    if (spec.language == "r" || spec.language == "R") return "r";
    if (!spec.language.empty()) return spec.language;
    return spec.name;
}

namespace {
std::string LowerAscii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

std::string NotebookDefaultKernel(const NotebookDoc &doc, const std::vector<NotebookKernelSpec> &specs) {
    const Json &kernelspec = doc.metadata.get("kernelspec");
    std::string name = kernelspec.get("name").as_string("");
    if (!name.empty() && FindNotebookKernel(specs, name)) return name;
    std::string language = LowerAscii(kernelspec.get("language").as_string(""));
    if (language.empty()) language = LowerAscii(doc.metadata.get("language_info").get("name").as_string(""));
    if (!language.empty()) {
        for (const NotebookKernelSpec &s : specs) {
            if (LowerAscii(NotebookKernelLanguageName(s)) == language || LowerAscii(s.name) == language) return s.name;
        }
    }
    if (!specs.empty()) return specs.front().name;
    return "python3";
}

void NotebookAppendScriptExit(NotebookCell *cell, int exit_code, const std::string &command) {
    if (exit_code == 0) return;
    NotebookOutput err;
    err.kind = NotebookOutput::Kind::Error;
    if (exit_code < 0) {
        err.ename = "KernelError";
        err.evalue = "could not start " + command;
    } else {
        err.ename = "ExitStatus";
        err.evalue = command + " exited with status " + std::to_string(exit_code);
    }
    err.text = err.ename + ": " + err.evalue;
    NotebookAppendOutput(cell, std::move(err));
}

// --- Output display ------------------------------------------------------

std::string StripAnsi(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7E)) j++;
            i = j;  // skip the final byte too
            continue;
        }
        if (s[i] != '\r') out += s[i];
    }
    return out;
}

std::vector<std::string> NotebookOutputDisplayLines(const NotebookOutput &out) {
    std::vector<std::string> lines;
    if (!out.image_png.empty()) return lines;
    std::string text = StripAnsi(out.text);
    if (text.empty()) return lines;
    size_t start = 0;
    int total = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        bool last = nl == std::string::npos;
        // A stream's trailing newline (print's own) isn't an extra blank
        // display line.
        if (!(last && line.empty() && total > 0)) {
            if (total < kNotebookMaxOutputLines) lines.push_back(line);
            total++;
        }
        if (last) break;
        start = nl + 1;
    }
    if (total > kNotebookMaxOutputLines) {
        lines.push_back("... (" + std::to_string(total - kNotebookMaxOutputLines) + " more lines)");
    }
    return lines;
}

int NotebookImageSlots(int width, int height, double char_aspect) {
    if (width <= 0 || height <= 0) return 20;
    if (char_aspect <= 0.0) char_aspect = kNotebookDefaultCharAspect;
    // Height at kNotebookImageWidthChars wide, in line-heights.
    double h_chars = static_cast<double>(kNotebookImageWidthChars) * char_aspect * static_cast<double>(height) /
                     static_cast<double>(width);
    int slots = static_cast<int>(h_chars + 0.999);
    return std::max(kNotebookImageMinSlots, std::min(kNotebookImageMaxSlots, slots));
}

int NotebookOutputSlots(const NotebookOutput &out, double char_aspect) {
    if (!out.image_png.empty()) return NotebookImageSlots(out.image_width, out.image_height, char_aspect);
    return static_cast<int>(NotebookOutputDisplayLines(out).size());
}

int NotebookCellOutputSlots(const NotebookCell &cell, double char_aspect) {
    if (cell.type != NotebookCellType::Code) return 0;
    int total = 0;
    for (const NotebookOutput &o : cell.outputs) total += NotebookOutputSlots(o, char_aspect);
    return total;
}

bool PngDimensionsFromBase64(const std::string &b64, int *width, int *height) {
    std::vector<unsigned char> head = Base64Prefix(b64, 24);
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (head.size() < 24) return false;
    for (int i = 0; i < 8; i++) {
        if (head[static_cast<size_t>(i)] != kSig[i]) return false;
    }
    if (head[12] != 'I' || head[13] != 'H' || head[14] != 'D' || head[15] != 'R') return false;
    auto be32 = [&](size_t at) {
        return (static_cast<uint32_t>(head[at]) << 24) | (static_cast<uint32_t>(head[at + 1]) << 16) |
               (static_cast<uint32_t>(head[at + 2]) << 8) | static_cast<uint32_t>(head[at + 3]);
    };
    uint32_t w = be32(16), h = be32(20);
    if (w == 0 || h == 0 || w > 65535 || h > 65535) return false;
    if (width) *width = static_cast<int>(w);
    if (height) *height = static_cast<int>(h);
    return true;
}

std::string NotebookContentKey(const std::string &data) {
    uint64_t hash = 1469598103934665603ULL;
    for (char raw : data) {
        hash ^= static_cast<unsigned char>(raw);
        hash *= 1099511628211ULL;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

void NotebookAppendOutput(NotebookCell *cell, NotebookOutput out) {
    if (out.kind == NotebookOutput::Kind::Stream && !cell->outputs.empty()) {
        NotebookOutput &last = cell->outputs.back();
        if (last.kind == NotebookOutput::Kind::Stream && last.name == out.name && last.raw.is_null()) {
            if (last.text.size() >= kNotebookMaxStreamBytes) return;  // already truncated
            last.text += out.text;
            if (last.text.size() > kNotebookMaxStreamBytes) {
                last.text.resize(kNotebookMaxStreamBytes);
                last.text += "\n[output truncated]\n";
            }
            return;
        }
    }
    if (out.kind == NotebookOutput::Kind::Stream && out.text.size() > kNotebookMaxStreamBytes) {
        out.text.resize(kNotebookMaxStreamBytes);
        out.text += "\n[output truncated]\n";
    }
    cell->outputs.push_back(std::move(out));
}

// --- Kernel wire protocol ------------------------------------------------

bool ParseNotebookKernelMessage(const std::string &line, NotebookKernelMessage *out) {
    Json j;
    if (!Json::Parse(line, &j) || !j.is_object()) return false;
    NotebookKernelMessage m;
    m.type = j.get("type").as_string("");
    if (m.type.empty()) return false;
    if (j.get("id").is_number()) m.id = j.get("id").as_int();
    m.name = j.get("name").as_string("");
    m.text = j.get("text").as_string("");
    m.png = StripWhitespace(j.get("png").as_string(""));
    m.ename = j.get("ename").as_string("");
    m.evalue = j.get("evalue").as_string("");
    for (const Json &t : j.get("traceback").items()) {
        if (t.is_string()) m.traceback.push_back(t.as_string());
    }
    if (j.get("execution_count").is_number()) m.execution_count = j.get("execution_count").as_int();
    m.python = j.get("python").as_string("");
    *out = std::move(m);
    return true;
}

std::string NotebookKernelExecuteRequest(int id, const std::string &code) {
    Json j = Json::Object();
    j["op"] = Json("execute");
    j["id"] = Json(id);
    j["code"] = Json(code);
    return j.dump() + "\n";
}

std::string NotebookKernelShutdownRequest() { return "{\"op\":\"shutdown\"}\n"; }

const char *NotebookKernelScript() {
    // Raw literal: no C escapes to fight, and no trigraph risk. Kept
    // dependency-free (stdlib only); matplotlib is optional and only
    // touched once user code has imported it.
    static const char *const kScript = R"PYKERNEL(
import sys, os, io, json, ast, traceback, base64, builtins, tempfile

_out = sys.stdout
_in = sys.stdin
_exec_count = 0


def _emit(msg):
    _out.write(json.dumps(msg) + "\n")
    _out.flush()


class _Stream(io.TextIOBase):
    def __init__(self, name):
        self._name = name
        self.id = -1
        self._buf = ""

    def writable(self):
        return True

    def isatty(self):
        return False

    @property
    def encoding(self):
        return "utf-8"

    def write(self, s):
        if not isinstance(s, str):
            s = str(s)
        self._buf += s
        if "\n" in s or len(self._buf) > 4096:
            self.flush()
        return len(s)

    def flush(self):
        if self._buf:
            _emit({"type": "stream", "id": self.id, "name": self._name, "text": self._buf})
            self._buf = ""


class _NoStdin(io.TextIOBase):
    def readable(self):
        return True

    def read(self, *args):
        raise EOFError("stdin is not available in a mep notebook cell")

    readline = read


_stdout = _Stream("stdout")
_stderr = _Stream("stderr")
sys.stdout = _stdout
sys.stderr = _stderr
sys.stdin = _NoStdin()

_ns = {"__name__": "__main__", "__builtins__": builtins}

# Inline matplotlib backend: a real module on sys.path (matplotlib's
# module:// loader needs an importable module, not an in-memory object),
# whose show() hands every open figure back to this kernel as PNG.
_backend_dir = tempfile.mkdtemp(prefix="mep_nb_")
with open(os.path.join(_backend_dir, "mep_nb_backend.py"), "w") as _f:
    _f.write(
        "from matplotlib.backends.backend_agg import FigureCanvasAgg\n"
        "from matplotlib.backend_bases import FigureManagerBase\n"
        "import builtins\n"
        "FigureCanvas = FigureCanvasAgg\n"
        "FigureManager = FigureManagerBase\n"
        "def show(*args, **kwargs):\n"
        "    hook = getattr(builtins, '_mep_nb_show_hook', None)\n"
        "    if hook is not None:\n"
        "        hook()\n"
    )
sys.path.insert(0, _backend_dir)
os.environ["MPLBACKEND"] = "module://mep_nb_backend"


def _figure_png(fig):
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=100, bbox_inches="tight", facecolor=fig.get_facecolor())
    return base64.b64encode(buf.getvalue()).decode("ascii")


def _capture_figures():
    if "matplotlib" not in sys.modules:
        return
    try:
        from matplotlib._pylab_helpers import Gcf
    except Exception:
        return
    managers = list(Gcf.get_all_fig_managers())
    for manager in managers:
        fig = manager.canvas.figure
        try:
            if fig.get_axes():
                _emit({"type": "display_data", "id": _stdout.id, "png": _figure_png(fig), "text": None})
        except Exception as e:
            _stderr.write("mep notebook: could not render figure: %r\n" % (e,))
    Gcf.destroy_all()


builtins._mep_nb_show_hook = _capture_figures


def _emit_result(rid, value):
    png = None
    try:
        repr_png = getattr(value, "_repr_png_", None)
        if callable(repr_png):
            data = repr_png()
            if isinstance(data, str):
                png = data
            elif data:
                png = base64.b64encode(data).decode("ascii")
        elif "matplotlib" in sys.modules:
            import matplotlib.figure
            if isinstance(value, matplotlib.figure.Figure):
                png = _figure_png(value)
                # Shown once, as this result -- not again by the
                # end-of-cell capture (real Jupyter shows it twice here).
                from matplotlib._pylab_helpers import Gcf
                Gcf.destroy_fig(value)
    except Exception:
        png = None
    try:
        text = repr(value)
    except Exception as e:
        text = "<repr failed: %r>" % (e,)
    _emit({"type": "execute_result", "id": rid, "text": text, "png": png})


def _format_error(etype, evalue, tb):
    while tb is not None and tb.tb_frame.f_code.co_filename != "<cell>":
        tb = tb.tb_next
    if tb is None:
        lines = traceback.format_exception_only(etype, evalue)
    else:
        lines = traceback.format_exception(etype, evalue, tb)
    out = []
    for chunk in lines:
        out.extend(chunk.rstrip("\n").split("\n"))
    return out


def _run(req):
    global _exec_count
    rid = req.get("id", -1)
    code = req.get("code", "")
    _stdout.id = rid
    _stderr.id = rid
    _exec_count += 1
    try:
        tree = ast.parse(code, filename="<cell>", mode="exec")
        last = None
        if tree.body and isinstance(tree.body[-1], ast.Expr) and not code.rstrip().endswith(";"):
            last = ast.Expression(tree.body[-1].value)
            ast.fix_missing_locations(last)
            tree.body = tree.body[:-1]
        exec(compile(tree, "<cell>", "exec"), _ns)
        if last is not None:
            value = eval(compile(last, "<cell>", "eval"), _ns)
            if value is not None:
                _ns["_"] = value
                _emit_result(rid, value)
    except BaseException:
        etype, evalue, tb = sys.exc_info()
        _stdout.flush()
        _emit({
            "type": "error", "id": rid,
            "ename": etype.__name__, "evalue": str(evalue),
            "traceback": _format_error(etype, evalue, tb),
        })
    finally:
        try:
            _capture_figures()
        except BaseException as e:
            _stderr.write("mep notebook: figure capture failed: %r\n" % (e,))
        _stdout.flush()
        _stderr.flush()
    _emit({"type": "done", "id": rid, "execution_count": _exec_count})


_emit({"type": "ready", "python": sys.version.split()[0]})
while True:
    try:
        line = _in.readline()
    except KeyboardInterrupt:
        continue
    if not line:
        break
    line = line.strip()
    if not line:
        continue
    try:
        req = json.loads(line)
    except Exception:
        continue
    op = req.get("op")
    if op == "execute":
        _run(req)
    elif op == "shutdown":
        break
)PYKERNEL";
    return kScript;
}

const char *NotebookRKernelScript() {
    // Raw literal, like NotebookKernelScript(). Needs only base R plus
    // jsonlite (which rmarkdown itself depends on, so any setup that can
    // knit an .Rmd has it); exits with a stderr line naming it otherwise,
    // which KernelProc::last_error puts on the status line.
    static const char *const kScript = R"RKERNEL(
local({
    if (!requireNamespace("jsonlite", quietly = TRUE)) {
        cat("mep R kernel: the jsonlite package is required (install.packages('jsonlite'))\n", file = stderr())
        quit(save = "no", status = 1)
    }
    # The protocol goes to the process's real stdout. User output goes to
    # R's stdout() connection, which each run sinks into a text
    # connection, so a cat() in a chunk can never corrupt a protocol line.
    proto <- file("/dev/stdout", "w", raw = TRUE)
    input <- file("stdin", "r")
    emit <- function(msg) {
        writeLines(jsonlite::toJSON(msg, auto_unbox = TRUE, null = "null", digits = NA), proto)
        flush(proto)
    }
    count <- 0L
    fig_dir <- ""
    png_type <- if (isTRUE(capabilities("cairo"))) "cairo" else getOption("bitmapType")
    open_device <- function() {
        grDevices::png(file.path(fig_dir, "fig_%03d.png"), width = 7, height = 5, units = "in",
                       res = 100, type = png_type)
    }
    # A plot after the chunk's own dev.off() would otherwise open
    # Rplots.pdf; route any new device into this run's figure folder.
    options(device = function(...) open_device(), width = 80)

    run <- function(id, code) {
        count <<- count + 1L
        fig_dir <<- file.path(tempdir(), sprintf("mep_fig_%d", id))
        dir.create(fig_dir, showWarnings = FALSE)
        open_device()
        captured <- character()
        out <- textConnection("captured", "w", local = TRUE)
        sink(out)
        sent <- 0L
        flush_out <- function(final = FALSE) {
            if (final) {
                while (sink.number() > 0) sink()
                close(out)
            }
            if (length(captured) > sent) {
                text <- paste0(paste(captured[(sent + 1L):length(captured)], collapse = "\n"), "\n")
                sent <<- length(captured)
                emit(list(type = "stream", id = id, name = "stdout", text = text))
            }
        }
        stderr_text <- function(text) {
            flush_out()
            emit(list(type = "stream", id = id, name = "stderr", text = text))
        }
        report_error <- function(e) {
            flush_out()
            msg <- conditionMessage(e)
            call <- conditionCall(e)
            # A top-level stop() reports the driver's own eval() as its
            # call; R's console says plain "Error:" there, and so does this.
            if (is.call(call) && identical(call[[1]], quote(eval))) call <- NULL
            head <- if (is.null(call)) "Error: " else paste0("Error in ", deparse(call, nlines = 1L), ": ")
            emit(list(type = "error", id = id, ename = "Error", evalue = msg, traceback = list(paste0(head, msg))))
        }
        exprs <- tryCatch(parse(text = code, keep.source = FALSE), error = function(e) {
            report_error(simpleError(conditionMessage(e)))
            NULL
        })
        for (ex in exprs) {
            ok <- withCallingHandlers(
                tryCatch({
                    r <- withVisible(eval(ex, envir = globalenv()))
                    if (r$visible) print(r$value)
                    TRUE
                }, error = function(e) {
                    report_error(e)
                    FALSE
                }, interrupt = function(i) {
                    flush_out()
                    emit(list(type = "error", id = id, ename = "Interrupted", evalue = "",
                              traceback = list("Interrupted")))
                    FALSE
                }),
                message = function(m) {
                    stderr_text(conditionMessage(m))
                    invokeRestart("muffleMessage")
                },
                warning = function(w) {
                    stderr_text(paste0("Warning: ", conditionMessage(w), "\n"))
                    invokeRestart("muffleWarning")
                })
            if (!isTRUE(ok)) break
        }
        flush_out(final = TRUE)
        grDevices::graphics.off()
        for (f in sort(list.files(fig_dir, pattern = "\\.png$", full.names = TRUE))) {
            size <- file.info(f)$size
            if (!is.na(size) && size > 0) {
                emit(list(type = "display_data", id = id, text = NULL,
                          png = jsonlite::base64_enc(readBin(f, "raw", size))))
            }
        }
        unlink(fig_dir, recursive = TRUE)
        emit(list(type = "done", id = id, execution_count = count))
    }

    emit(list(type = "ready", python = R.version.string))
    repeat {
        line <- tryCatch(readLines(input, n = 1L, warn = FALSE), interrupt = function(i) "")
        if (length(line) == 0L) break
        if (!nzchar(line)) next
        req <- tryCatch(jsonlite::fromJSON(line, simplifyVector = FALSE), error = function(e) NULL)
        if (is.null(req)) next
        if (identical(req$op, "shutdown")) break
        if (identical(req$op, "execute")) run(as.integer(req$id), paste(unlist(req$code), collapse = "\n"))
    }
    quit(save = "no")
})
)RKERNEL";
    return kScript;
}

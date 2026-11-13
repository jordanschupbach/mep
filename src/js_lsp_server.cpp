// `mep-js-lsp`: the wire half of mep's own JavaScript language server, and
// a command-line linter. Speaks LSP over stdio; every answer comes from
// js_lsp.h's pure analysis functions.
//
//   mep-js-lsp                    the language server on stdio
//   mep-js-lsp --lint FILE...     diagnostics as path:line:col: severity: message [code];
//                                    exit 1 on any error or warning ('-' reads stdin)
//
// Single-threaded and synchronous; Content-Length framing via
// rpc_framing.h, full text sync. Positions are UTF-8 byte columns
// (`positionEncoding: "utf-8"`, what mep's client speaks) unless the client
// lists `general.positionEncodings` without "utf-8" -- then UTF-16, LSP's
// default, converted line by line here. The dialect (module/script, JSX,
// later TypeScript) comes from the document's file extension.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "js_format.h"
#include "js_lsp.h"
#include "json.h"
#include "rpc_framing.h"

namespace {

/** @brief The filesystem path of a `file://` URI (percent-decoded), "" for any other scheme. */
std::string UriToPath(const std::string &uri) {
    if (uri.rfind("file://", 0) != 0) return std::string();
    std::string out;
    for (size_t i = 7; i < uri.size(); i++) {
        if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
            out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += uri[i];
        }
    }
#if defined(_WIN32)
    if (out.size() > 2 && out[0] == '/' && out[2] == ':') out.erase(0, 1);  // file:///C:/x
#endif
    return out;
}

std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    lines.push_back(cur);
    return lines;
}

std::string ReadFile(const std::string &path, bool *ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// UTF-16 <-> UTF-8 columns on one line.
int ByteColumn(const std::string &line, int u16) {
    int units = 0;
    size_t i = 0;
    while (i < line.size() && units < u16) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        units += len == 4 ? 2 : 1;
        i += len;
    }
    return static_cast<int>(std::min(i, line.size()));
}

int U16Column(const std::string &line, int bytes) {
    int units = 0;
    size_t i = 0;
    const size_t end = std::min(line.size(), static_cast<size_t>(std::max(0, bytes)));
    while (i < end) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        units += len == 4 ? 2 : 1;
        i += len;
    }
    return units;
}

// The conversion state for the message being answered: the document's lines, and whether the
// client counts in UTF-16.
bool g_utf16 = false;
const std::vector<std::string> *g_lines = nullptr;

int OutColumn(int line, int col) {
    if (!g_utf16 || !g_lines || line < 0 || static_cast<size_t>(line) >= g_lines->size()) return col;
    return U16Column((*g_lines)[static_cast<size_t>(line)], col);
}

int InColumn(int line, int col) {
    if (!g_utf16 || !g_lines || line < 0 || static_cast<size_t>(line) >= g_lines->size()) return col;
    return ByteColumn((*g_lines)[static_cast<size_t>(line)], col);
}

Json Position(int line, int character) {
    Json p = Json::Object();
    p["line"] = line;
    p["character"] = OutColumn(line, character);
    return p;
}

Json RangeOf(const JsLspRange &range) {
    Json r = Json::Object();
    r["start"] = Position(range.line, range.col);
    r["end"] = Position(range.end_line, range.end_col);
    return r;
}

Json EditsJson(const std::vector<JsLspTextEdit> &edits) {
    Json arr = Json::Array();
    for (const JsLspTextEdit &e : edits) {
        Json j = Json::Object();
        j["range"] = RangeOf(e.range);
        j["newText"] = e.new_text;
        arr.push_back(j);
    }
    return arr;
}

// Formatting: mep's JavaScript formatter (js_format.h, a port of Prettier),
// as one edit over the lines that change.
constexpr bool kHasFormatter = true;

// The dialect comes from the document's URI (.ts/.mts/.cts TypeScript, .d.ts a declaration file,
// .tsx TypeScript + JSX, anything else JavaScript + JSX). Of the request's FormattingOptions only
// insertSpaces: false (tabs) is taken. tabSize is not: mep (like most clients) sends one fixed
// value for every language, and Prettier's width is 2 unless a project says otherwise.
std::vector<JsLspTextEdit> FormatDocument(const std::vector<std::string> &lines, const std::string &uri,
                                          const Json &options, std::string *error) {
    std::vector<JsLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    jsfmt::Options o;
    jsfmt::SetDialectForPath(o, uri);
    if (options.get("insertSpaces").is_bool()) o.use_tabs = !options.get("insertSpaces").as_bool();
    const jsfmt::Result r = jsfmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    // The formatter ends the file with a newline; the lines carry one only
    // as an empty last line.
    std::string out = r.text;
    if (!out.empty() && out.back() == '\n' && (text.empty() || text.back() != '\n')) out.pop_back();
    if (out == text) return edits;
    std::vector<std::string> b;
    for (size_t i = 0;;) {
        const size_t j = out.find('\n', i);
        if (j == std::string::npos) {
            b.push_back(out.substr(i));
            break;
        }
        b.push_back(out.substr(i, j - i));
        i = j + 1;
    }
    size_t pre = 0;
    while (pre < lines.size() && pre < b.size() && lines[pre] == b[pre]) pre++;
    size_t suf = 0;
    while (suf < lines.size() - pre && suf < b.size() - pre && lines[lines.size() - 1 - suf] == b[b.size() - 1 - suf])
        suf++;
    JsLspTextEdit e;
    e.range.line = static_cast<int>(pre);
    const size_t a_end = lines.size() - suf;
    const size_t b_end = b.size() - suf;
    if (a_end < lines.size()) {
        e.range.end_line = static_cast<int>(a_end);
        for (size_t k = pre; k < b_end; k++) e.new_text += b[k] + "\n";
    } else {
        // The change reaches the last line: replace up to its end.
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        for (size_t k = pre; k < b_end; k++) {
            if (k > pre) e.new_text += "\n";
            e.new_text += b[k];
        }
        if (pre > 0 && pre == lines.size() && b_end > pre) {
            e.range.line = static_cast<int>(pre) - 1;
            e.range.col = static_cast<int>(lines[pre - 1].size());
            e.new_text = "\n" + e.new_text;
        }
    }
    edits.push_back(e);
    return edits;
}

class JsLanguageServer {
public:
    void HandleMessage(const std::string &body) {
        Json msg;
        if (!Json::Parse(body, &msg)) return;
        const std::string method = msg.get("method").as_string();
        const Json &params = msg.get("params");
        const bool is_request = msg.contains("id");

        if (method == "initialize") {
            const Json &encs = params.get("capabilities").get("general").get("positionEncodings");
            if (encs.is_array() && encs.size() > 0) {
                utf16_ = true;
                for (const Json &e : encs.items()) {
                    if (e.as_string() == "utf-8") utf16_ = false;
                }
            }
            Reply(msg.get("id"), Capabilities(utf16_));
            return;
        }
        if (method == "shutdown") {
            shutting_down_ = true;
            Reply(msg.get("id"), Json());
            return;
        }
        if (method == "exit") {
            exit_code_ = shutting_down_ ? 0 : 1;
            running_ = false;
            return;
        }
        if (method == "initialized" || method == "workspace/didChangeConfiguration" || method == "$/setTrace" ||
            method == "textDocument/didSave") {
            return;
        }
        if (method == "textDocument/didOpen") {
            const std::string uri = UriOf(params);
            Set(uri, params.get("textDocument").get("text").as_string());
            Publish(uri);
            return;
        }
        if (method == "textDocument/didChange") {
            const std::string uri = UriOf(params);
            const Json &changes = params.get("contentChanges");
            if (changes.is_array() && changes.size() > 0) Set(uri, changes.items().back().get("text").as_string());
            Publish(uri);
            return;
        }
        if (method == "textDocument/didClose") {
            const std::string uri = UriOf(params);
            docs_.erase(uri);
            Json note = Json::Object();
            note["uri"] = uri;
            note["diagnostics"] = Json::Array();
            Notify("textDocument/publishDiagnostics", note);
            return;
        }
        if (!is_request) return;
        g_utf16 = utf16_;
        g_lines = params.contains("textDocument") ? &Doc(UriOf(params)).lines : nullptr;

        if (method == "textDocument/completion") {
            Reply(msg.get("id"), Completion(params));
        } else if (method == "textDocument/hover") {
            Reply(msg.get("id"), HoverAt(params));
        } else if (method == "textDocument/documentSymbol") {
            Reply(msg.get("id"), Symbols(params));
        } else if (method == "textDocument/foldingRange") {
            Reply(msg.get("id"), Folds(params));
        } else if (method == "textDocument/definition") {
            Reply(msg.get("id"), Definition(params));
        } else if (method == "textDocument/references") {
            Reply(msg.get("id"), References(params));
        } else if (method == "textDocument/documentHighlight") {
            Reply(msg.get("id"), Highlights(params));
        } else if (method == "textDocument/signatureHelp") {
            Reply(msg.get("id"), SignatureHelp(params));
        } else if (method == "textDocument/prepareRename") {
            Reply(msg.get("id"), PrepareRename(params));
        } else if (method == "textDocument/rename") {
            std::string error;
            Json result = Rename(params, &error);
            if (!error.empty()) {
                ReplyError(msg.get("id"), -32803, error);
            } else {
                Reply(msg.get("id"), result);
            }
        } else if (method == "textDocument/formatting" || method == "textDocument/rangeFormatting") {
            std::string error;
            const std::vector<JsLspTextEdit> edits = FormatDocument(Doc(UriOf(params)).lines, UriOf(params), params.get("options"), &error);
            if (!error.empty()) {
                ReplyError(msg.get("id"), -32803, error);
            } else {
                Reply(msg.get("id"), EditsJson(edits));
            }
        } else if (method == "completionItem/resolve") {
            Reply(msg.get("id"), params);
        } else {
            ReplyError(msg.get("id"), -32601, "Unsupported method: " + method);
        }
    }

    bool running() const { return running_; }
    int exit_code() const { return exit_code_; }

private:
    struct OpenDoc {
        std::vector<std::string> lines;
        std::shared_ptr<const JsLspDocument> analysis;
    };
    std::map<std::string, OpenDoc> docs_;
    bool running_ = true;
    bool utf16_ = false;
    bool shutting_down_ = false;
    int exit_code_ = 1;

    static void Send(const Json &msg) {
        const std::string framed = FrameRpcMessage(msg.dump());
        std::fwrite(framed.data(), 1, framed.size(), stdout);
        std::fflush(stdout);
    }

    static void Reply(const Json &id, Json result) {
        Json msg = Json::Object();
        msg["jsonrpc"] = "2.0";
        msg["id"] = id;
        msg["result"] = std::move(result);
        Send(msg);
    }

    static void ReplyError(const Json &id, int code, const std::string &message) {
        Json err = Json::Object();
        err["code"] = code;
        err["message"] = message;
        Json msg = Json::Object();
        msg["jsonrpc"] = "2.0";
        msg["id"] = id;
        msg["error"] = std::move(err);
        Send(msg);
    }

    static void Notify(const std::string &method, Json params) {
        Json msg = Json::Object();
        msg["jsonrpc"] = "2.0";
        msg["method"] = method;
        msg["params"] = std::move(params);
        Send(msg);
    }

    static Json Capabilities(bool utf16) {
        Json sync = Json::Object();
        sync["openClose"] = true;
        sync["change"] = 1;
        sync["save"] = false;

        Json completion = Json::Object();
        completion["resolveProvider"] = false;
        Json completion_triggers = Json::Array();
        completion_triggers.push_back(".");
        completion["triggerCharacters"] = completion_triggers;

        Json signature = Json::Object();
        Json signature_triggers = Json::Array();
        for (const char *t : {"(", ","}) signature_triggers.push_back(t);
        signature["triggerCharacters"] = signature_triggers;

        Json rename = Json::Object();
        rename["prepareProvider"] = true;

        Json caps = Json::Object();
        caps["positionEncoding"] = utf16 ? "utf-16" : "utf-8";
        caps["textDocumentSync"] = sync;
        caps["completionProvider"] = completion;
        caps["signatureHelpProvider"] = signature;
        caps["hoverProvider"] = true;
        caps["documentSymbolProvider"] = true;
        caps["foldingRangeProvider"] = true;
        caps["definitionProvider"] = true;
        caps["referencesProvider"] = true;
        caps["documentHighlightProvider"] = true;
        caps["renameProvider"] = rename;
        caps["documentFormattingProvider"] = kHasFormatter;

        Json info = Json::Object();
        info["name"] = "mep-js-lsp";
        info["version"] = "1";

        Json result = Json::Object();
        result["capabilities"] = caps;
        result["serverInfo"] = info;
        return result;
    }

    void Set(const std::string &uri, const std::string &text) {
        OpenDoc &d = docs_[uri];
        d.lines = SplitLines(text);
        d.analysis = JsLspAnalyze(d.lines, JsLspDialectForPath(uri), UriToPath(uri));
    }

    const OpenDoc &Doc(const std::string &uri) {
        const auto it = docs_.find(uri);
        if (it != docs_.end()) return it->second;
        Set(uri, std::string());
        return docs_[uri];
    }
    const JsLspDocument &Analysis(const Json &params) { return *Doc(UriOf(params)).analysis; }

    static std::string UriOf(const Json &params) { return params.get("textDocument").get("uri").as_string(); }
    static int LineOf(const Json &params) { return params.get("position").get("line").as_int(); }
    static int ColOf(const Json &params) { return InColumn(LineOf(params), params.get("position").get("character").as_int()); }

    void Publish(const std::string &uri) {
        g_utf16 = utf16_;
        g_lines = &Doc(uri).lines;
        Json arr = Json::Array();
        for (const JsLspDiagnostic &d : JsLspDiagnostics(*Doc(uri).analysis)) {
            Json j = Json::Object();
            j["range"] = RangeOf(d.range);
            j["severity"] = static_cast<int>(d.severity);
            j["code"] = d.code;
            j["source"] = "javascript";
            j["message"] = d.message;
            if (d.unnecessary || d.deprecated) {
                Json tags = Json::Array();
                if (d.unnecessary) tags.push_back(1);
                if (d.deprecated) tags.push_back(2);
                j["tags"] = tags;
            }
            arr.push_back(j);
        }
        Json note = Json::Object();
        note["uri"] = uri;
        note["diagnostics"] = arr;
        Notify("textDocument/publishDiagnostics", note);
    }

    Json Completion(const Json &params) {
        const int line = LineOf(params);
        const std::vector<JsLspCompletionItem> items = JsLspCompletions(Analysis(params), line, ColOf(params));
        Json arr = Json::Array();
        for (size_t i = 0; i < items.size(); i++) {
            const JsLspCompletionItem &it = items[i];
            Json j = Json::Object();
            j["label"] = it.label;
            j["kind"] = static_cast<int>(it.kind);
            j["insertText"] = it.insert_text;
            j["insertTextFormat"] = 1;
            if (!it.detail.empty()) j["detail"] = it.detail;
            if (!it.documentation.empty()) {
                Json doc = Json::Object();
                doc["kind"] = "markdown";
                doc["value"] = it.documentation;
                j["documentation"] = doc;
            }
            std::string sort_text = std::to_string(i);
            if (sort_text.size() < 4) sort_text.insert(0, 4 - sort_text.size(), '0');
            j["sortText"] = sort_text;
            Json range = Json::Object();
            range["start"] = Position(line, it.replace_start);
            range["end"] = Position(line, it.replace_end);
            Json edit = Json::Object();
            edit["range"] = range;
            edit["newText"] = it.insert_text;
            j["textEdit"] = edit;
            arr.push_back(j);
        }
        Json list = Json::Object();
        list["isIncomplete"] = false;
        list["items"] = arr;
        return list;
    }

    Json HoverAt(const Json &params) {
        const JsLspHoverInfo info = JsLspHover(Analysis(params), LineOf(params), ColOf(params));
        if (!info.found) return Json();
        Json contents = Json::Object();
        contents["kind"] = "markdown";
        contents["value"] = info.text;
        Json out = Json::Object();
        out["contents"] = contents;
        out["range"] = RangeOf(info.range);
        return out;
    }

    Json Symbols(const Json &params) {
        const std::vector<JsLspSymbol> syms = JsLspSymbols(Analysis(params));
        std::vector<Json> nodes(syms.size());
        for (size_t i = syms.size(); i-- > 0;) {
            const JsLspSymbol &s = syms[i];
            Json j = Json::Object();
            j["name"] = s.name;
            if (!s.detail.empty()) j["detail"] = s.detail;
            j["kind"] = static_cast<int>(s.kind);
            j["range"] = RangeOf(s.range);
            j["selectionRange"] = RangeOf(s.sel_range);
            if (!nodes[i].is_null()) j["children"] = nodes[i];
            if (s.parent >= 0 && static_cast<size_t>(s.parent) < nodes.size()) {
                nodes[static_cast<size_t>(s.parent)].push_back(j);
            } else {
                nodes[i] = j;
            }
        }
        Json arr = Json::Array();
        for (size_t i = 0; i < syms.size(); i++) {
            if (syms[i].parent < 0 && nodes[i].is_object()) arr.push_back(nodes[i]);
        }
        return arr;
    }

    Json Folds(const Json &params) {
        Json arr = Json::Array();
        for (const JsLspFold &f : JsLspFolds(Analysis(params))) {
            Json j = Json::Object();
            j["startLine"] = f.start_line;
            j["endLine"] = f.end_line;
            if (!f.kind.empty()) j["kind"] = f.kind;
            arr.push_back(j);
        }
        return arr;
    }

    Json Definition(const Json &params) {
        const std::string uri = UriOf(params);
        Json arr = Json::Array();
        for (const JsLspLocation &loc : JsLspDefinition(Analysis(params), LineOf(params), ColOf(params))) {
            Json j = Json::Object();
            j["uri"] = uri;
            j["range"] = RangeOf(loc.range);
            arr.push_back(j);
        }
        if (arr.size() == 0) return Json();
        return arr;
    }

    Json References(const Json &params) {
        const std::string uri = UriOf(params);
        const bool include_declaration =
            !params.contains("context") || params.get("context").get("includeDeclaration").as_bool();
        Json arr = Json::Array();
        for (const JsLspReference &ref : JsLspReferences(Analysis(params), LineOf(params), ColOf(params))) {
            if (ref.is_definition && !include_declaration) continue;
            Json j = Json::Object();
            j["uri"] = uri;
            j["range"] = RangeOf(ref.range);
            arr.push_back(j);
        }
        return arr;
    }

    Json Highlights(const Json &params) {
        Json arr = Json::Array();
        for (const JsLspReference &ref : JsLspReferences(Analysis(params), LineOf(params), ColOf(params))) {
            Json j = Json::Object();
            j["range"] = RangeOf(ref.range);
            j["kind"] = ref.is_definition ? 3 : 2;
            arr.push_back(j);
        }
        return arr;
    }

    Json PrepareRename(const Json &params) {
        const JsLspRenameTarget t = JsLspPrepareRename(Analysis(params), LineOf(params), ColOf(params));
        if (!t.found) return Json();
        Json out = Json::Object();
        out["range"] = RangeOf(t.range);
        out["placeholder"] = t.placeholder;
        return out;
    }

    Json Rename(const Json &params, std::string *error) {
        const std::string uri = UriOf(params);
        const std::vector<JsLspTextEdit> edits =
            JsLspRename(Analysis(params), LineOf(params), ColOf(params), params.get("newName").as_string(), error);
        if (!error->empty()) return Json();
        Json changes = Json::Object();
        changes[uri] = EditsJson(edits);
        Json out = Json::Object();
        out["changes"] = changes;
        return out;
    }

    Json SignatureHelp(const Json &params) {
        const JsLspSignatureHelp sh = JsLspSignatureAt(Analysis(params), LineOf(params), ColOf(params));
        if (!sh.found) return Json();
        auto signature_json = [](const std::string &label, const std::vector<std::string> &param_labels,
                                 const std::string &documentation) {
            Json parameters = Json::Array();
            for (const std::string &param : param_labels) {
                Json p = Json::Object();
                p["label"] = param;
                parameters.push_back(p);
            }
            Json signature = Json::Object();
            signature["label"] = label;
            if (!documentation.empty()) {
                Json doc = Json::Object();
                doc["kind"] = "markdown";
                doc["value"] = documentation;
                signature["documentation"] = doc;
            }
            signature["parameters"] = parameters;
            return signature;
        };
        Json signatures = Json::Array();
        if (sh.overloads.empty()) {
            signatures.push_back(signature_json(sh.label, sh.params, sh.documentation));
        } else {
            // Every overload a declaration file gives, the one that fits the arguments so far active.
            for (const JsLspSignatureHelp::Overload &o : sh.overloads)
                signatures.push_back(signature_json(o.label, o.params, o.documentation));
        }
        Json out = Json::Object();
        out["signatures"] = signatures;
        out["activeSignature"] = sh.overloads.empty() ? 0 : sh.active_signature;
        out["activeParameter"] = sh.active_param;
        return out;
    }
};

int Lint(const std::vector<std::string> &paths) {
    bool failed = false;
    bool unreadable = false;
    for (const std::string &path : paths) {
        std::string text;
        if (path == "-") {
            text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        } else {
            bool ok = false;
            text = ReadFile(path, &ok);
            if (!ok) {
                std::fprintf(stderr, "mep-js-lsp: cannot read %s\n", path.c_str());
                unreadable = true;
                continue;
            }
        }
        const auto doc = JsLspAnalyze(SplitLines(text), JsLspDialectForPath(path), path == "-" ? std::string() : path);
        for (const JsLspDiagnostic &d : JsLspDiagnostics(*doc)) {
            std::printf("%s\n", JsLspFormatDiagnostic(path, d).c_str());
            if (JsLspDiagnosticFails(d)) failed = true;
        }
    }
    if (unreadable) return 2;
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (argc > 1) {
        const std::string first = argv[1];
        std::vector<std::string> rest;
        for (int i = 2; i < argc; i++) rest.emplace_back(argv[i]);
        if (first == "--lint") {
            if (rest.empty()) rest.emplace_back("-");
            return Lint(rest);
        }
        if (first == "--help" || first == "-h") {
            std::printf("usage: mep-js-lsp                  run the language server on stdio\n"
                        "       mep-js-lsp --lint FILE...   print diagnostics (FILE '-' reads stdin)\n");
            return 0;
        }
        if (first != "--stdio") {
            std::fprintf(stderr, "mep-js-lsp: unknown argument %s (try --help)\n", first.c_str());
            return 2;
        }
    }
    JsLanguageServer server;
    std::string buffer;
    char chunk[4096];
    while (server.running()) {
#if defined(_WIN32)
        const int got = _read(_fileno(stdin), chunk, static_cast<unsigned int>(sizeof(chunk)));
#else
        const ssize_t got = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (got < 0 && errno == EINTR) continue;
#endif
        if (got <= 0) break;
        buffer.append(chunk, static_cast<size_t>(got));
        if (!PumpRpcFrames(buffer, [&server](const std::string &body) { server.HandleMessage(body); })) break;
    }
    return server.exit_code();
}

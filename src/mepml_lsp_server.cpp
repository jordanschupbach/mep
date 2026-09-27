// `mep-mepml-lsp`: the wire half of mep's own mepml language server.
// Speaks LSP over stdio; every answer comes from mepml_lsp.h's pure
// analysis functions, which read the document with mepml_doc.h's parser.
//
// The same shape as mep-org-lsp (org_lsp_server.cpp), whose transport
// notes apply unchanged: single-threaded and synchronous, full text sync,
// Content-Length framing via rpc_framing.h, and positionEncoding "utf-8"
// (byte columns) because mep's own client reads `character` that way.
// Beyond org's feature set it answers references, document highlights,
// rename (citation keys and headings with the links to them), code
// actions and formatting (tables).

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "json.h"
#include "mepml_lsp.h"
#include "rpc_framing.h"

namespace {

// --- URI <-> path -----------------------------------------------------

/** @brief Percent-decodes a URI, leaving any malformed escape sequence as written. */
std::string PercentDecode(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) != 0 &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2])) != 0) {
            const std::string hex = s.substr(i + 1, 2);
            out += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

/** @brief Percent-encodes the characters a `file:` URI may not carry literally. */
std::string PercentEncodePath(const std::string &path) {
    static const char *const kHex = "0123456789ABCDEF";
    std::string out;
    for (char c : path) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool safe = std::isalnum(u) != 0 || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out += c;
        } else {
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 0x0F];
        }
    }
    return out;
}

/** @brief Converts a `file://` URI to a filesystem path (any other scheme is returned unchanged). */
std::string UriToPath(const std::string &uri) {
    if (uri.rfind("file://", 0) != 0) return uri;
    return PercentDecode(uri.substr(7));
}

/** @brief Converts a filesystem path to a `file://` URI. */
std::string PathToUri(const std::string &path) { return "file://" + PercentEncodePath(path); }

// --- Document store ---------------------------------------------------

/** @brief Splits a document's full text into lines, dropping a trailing CR from CRLF input. */
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

/** @brief Builds the analysis options for a document URI (its directory anchors relative path checks). */
MepmlLspOptions OptionsFor(const std::string &uri) {
    MepmlLspOptions opts;
    const std::string path = UriToPath(uri);
    if (!path.empty() && path[0] == '/') {
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        (void)parent;
        if (!ec) opts.doc_path = path;
    }
    // An unsaved/untitled document has no directory to resolve against,
    // and guessing the server's own cwd would report missing files that
    // are perfectly fine relative to wherever the document will be saved.
    opts.check_files = !opts.doc_path.empty();
    return opts;
}

// --- JSON shorthands --------------------------------------------------

/** @brief Builds an LSP Position object. */
Json Position(int line, int character) {
    Json p = Json::Object();
    p["line"] = line;
    p["character"] = character;
    return p;
}

/** @brief Builds an LSP Range object from a single line's column span. */
Json RangeOnLine(int line, int col_start, int col_end) {
    Json r = Json::Object();
    r["start"] = Position(line, col_start);
    r["end"] = Position(line, col_end);
    return r;
}

/** @brief Builds an LSP Range object spanning whole lines. */
Json RangeLines(int start_line, int end_line, int end_col) {
    Json r = Json::Object();
    r["start"] = Position(start_line, 0);
    r["end"] = Position(end_line, end_col);
    return r;
}

// --- Server -----------------------------------------------------------

/** @brief Builds an LSP Range spanning two positions. */
Json Range(int sl, int sc, int el, int ec) {
    Json r = Json::Object();
    r["start"] = Position(sl, sc);
    r["end"] = Position(el, ec);
    return r;
}

/** @brief A TextEdit as LSP JSON. */
Json EditJson(const MepmlLspTextEdit &e) {
    Json j = Json::Object();
    j["range"] = Range(e.start_line, e.start_col, e.end_line, e.end_col);
    j["newText"] = e.new_text;
    return j;
}

class MepmlLanguageServer {
public:
    /** @brief Feeds one complete JSON-RPC message body to the server. */
    void HandleMessage(const std::string &body) {
        Json msg;
        if (!Json::Parse(body, &msg)) return;
        const std::string method = msg.get("method").as_string();
        const Json &params = msg.get("params");
        const bool is_request = msg.contains("id");

        if (method == "initialize") {
            Reply(msg.get("id"), Capabilities());
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
        if (method == "initialized" || method == "workspace/didChangeConfiguration" || method == "$/setTrace") return;
        if (method == "textDocument/didSave") {
            // A saved file may be one another document @imports, and the
            // saved document's own files may have appeared: re-lint all.
            for (const auto &kv : docs_) Publish(kv.first);
            return;
        }
        if (method == "textDocument/didOpen") {
            const std::string uri = params.get("textDocument").get("uri").as_string();
            docs_[uri] = SplitLines(params.get("textDocument").get("text").as_string());
            Publish(uri);
            return;
        }
        if (method == "textDocument/didChange") {
            const std::string uri = params.get("textDocument").get("uri").as_string();
            const Json &changes = params.get("contentChanges");
            if (changes.is_array() && changes.size() > 0) docs_[uri] = SplitLines(changes.items().back().get("text").as_string());
            Publish(uri);
            return;
        }
        if (method == "textDocument/didClose") {
            const std::string uri = params.get("textDocument").get("uri").as_string();
            docs_.erase(uri);
            Json note = Json::Object();
            note["uri"] = uri;
            note["diagnostics"] = Json::Array();
            Notify("textDocument/publishDiagnostics", note);
            return;
        }
        if (!is_request) return;

        const Json &id = msg.get("id");
        if (method == "textDocument/completion") Reply(id, Completion(params));
        else if (method == "textDocument/hover") Reply(id, HoverAt(params));
        else if (method == "textDocument/documentSymbol") Reply(id, Symbols(params));
        else if (method == "textDocument/foldingRange") Reply(id, Folds(params));
        else if (method == "textDocument/definition") Reply(id, Definition(params));
        else if (method == "textDocument/references") Reply(id, References(params));
        else if (method == "textDocument/documentHighlight") Reply(id, Highlights(params));
        else if (method == "textDocument/prepareRename") Reply(id, PrepareRename(params));
        else if (method == "textDocument/rename") Rename(id, params);
        else if (method == "textDocument/codeAction") Reply(id, CodeActions(params));
        else if (method == "textDocument/formatting" || method == "textDocument/rangeFormatting") Reply(id, Format(params));
        else if (method == "completionItem/resolve") Reply(id, params);
        else ReplyError(id, -32601, "Unsupported method: " + method);
    }

    /** @brief Reports whether the server is still meant to read messages. */
    bool running() const { return running_; }
    /** @brief The process exit status the `exit` notification asked for. */
    int exit_code() const { return exit_code_; }

private:
    std::map<std::string, std::vector<std::string>> docs_;
    bool running_ = true;
    bool shutting_down_ = false;
    int exit_code_ = 1;

    struct Request {
        std::string uri;
        int line = 0;
        int col = 0;
    };
    static Request ReadRequest(const Json &params) {
        Request r;
        r.uri = params.get("textDocument").get("uri").as_string();
        r.line = params.get("position").get("line").as_int();
        r.col = params.get("position").get("character").as_int();
        return r;
    }

    /** @brief Writes one framed JSON-RPC message to stdout and flushes it. */
    static void Send(const Json &msg) {
        const std::string framed = FrameRpcMessage(msg.dump());
        std::fwrite(framed.data(), 1, framed.size(), stdout);
        std::fflush(stdout);
    }
    /** @brief Sends a successful JSON-RPC response. */
    static void Reply(const Json &id, Json result) {
        Json msg = Json::Object();
        msg["jsonrpc"] = "2.0";
        msg["id"] = id;
        msg["result"] = std::move(result);
        Send(msg);
    }
    /** @brief Sends a JSON-RPC error response. */
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
    /** @brief Sends a JSON-RPC notification. */
    static void Notify(const std::string &method, Json params) {
        Json msg = Json::Object();
        msg["jsonrpc"] = "2.0";
        msg["method"] = method;
        msg["params"] = std::move(params);
        Send(msg);
    }

    /** @brief Builds this server's `initialize` result. */
    static Json Capabilities() {
        Json sync = Json::Object();
        sync["openClose"] = true;
        sync["change"] = 1;  // TextDocumentSyncKind.Full
        sync["save"] = true;

        Json completion = Json::Object();
        completion["resolveProvider"] = false;
        Json triggers = Json::Array();
        for (const char *t : {"@", "\\", "{", "|", "#", "/", "`", ","}) triggers.push_back(t);
        completion["triggerCharacters"] = triggers;

        Json rename = Json::Object();
        rename["prepareProvider"] = true;
        Json actions = Json::Object();
        Json action_kinds = Json::Array();
        action_kinds.push_back("quickfix");
        action_kinds.push_back("source");
        actions["codeActionKinds"] = action_kinds;

        Json caps = Json::Object();
        caps["positionEncoding"] = "utf-8";
        caps["textDocumentSync"] = sync;
        caps["completionProvider"] = completion;
        caps["hoverProvider"] = true;
        caps["documentSymbolProvider"] = true;
        caps["foldingRangeProvider"] = true;
        caps["definitionProvider"] = true;
        caps["referencesProvider"] = true;
        caps["documentHighlightProvider"] = true;
        caps["renameProvider"] = rename;
        caps["codeActionProvider"] = actions;
        caps["documentFormattingProvider"] = true;

        Json info = Json::Object();
        info["name"] = "mep-mepml-lsp";
        info["version"] = "1";
        Json result = Json::Object();
        result["capabilities"] = caps;
        result["serverInfo"] = info;
        return result;
    }

    /** @brief Returns a tracked document's lines, or an empty document when the URI is unknown. */
    const std::vector<std::string> &Doc(const std::string &uri) const {
        static const std::vector<std::string> kEmpty{std::string()};
        const auto it = docs_.find(uri);
        return it == docs_.end() ? kEmpty : it->second;
    }

    /** @brief Lints a document and pushes the result to the client. */
    void Publish(const std::string &uri) {
        Json arr = Json::Array();
        for (const MepmlLspDiagnostic &d : MepmlLspDiagnostics(Doc(uri), OptionsFor(uri))) {
            Json j = Json::Object();
            j["range"] = RangeOnLine(d.line, d.col_start, d.col_end);
            j["severity"] = static_cast<int>(d.severity);
            j["code"] = d.code;
            j["source"] = "mepml";
            j["message"] = d.message;
            if (d.code == "unused-citation") {
                Json tags = Json::Array();
                tags.push_back(1);  // DiagnosticTag.Unnecessary
                j["tags"] = tags;
            } else if (d.code == "deprecated-directive") {
                Json tags = Json::Array();
                tags.push_back(2);  // DiagnosticTag.Deprecated
                j["tags"] = tags;
            }
            arr.push_back(j);
        }
        Json note = Json::Object();
        note["uri"] = uri;
        note["diagnostics"] = arr;
        Notify("textDocument/publishDiagnostics", note);
    }

    /** @brief Answers `textDocument/completion`. */
    Json Completion(const Json &params) const {
        const Request req = ReadRequest(params);
        Json arr = Json::Array();
        const std::vector<MepmlLspCompletionItem> items = MepmlLspCompletions(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        for (size_t i = 0; i < items.size(); i++) {
            const MepmlLspCompletionItem &it = items[i];
            Json j = Json::Object();
            j["label"] = it.label;
            j["kind"] = static_cast<int>(it.kind);
            j["insertText"] = it.insert_text;
            j["insertTextFormat"] = 1;
            if (!it.detail.empty()) j["detail"] = it.detail;
            if (!it.documentation.empty()) j["documentation"] = it.documentation;
            std::string sort_text = std::to_string(i);
            if (sort_text.size() < 4) sort_text.insert(0, 4 - sort_text.size(), '0');
            j["sortText"] = sort_text;
            Json edit = Json::Object();
            edit["range"] = RangeOnLine(req.line, it.replace_start, it.replace_end);
            edit["newText"] = it.insert_text;
            j["textEdit"] = edit;
            arr.push_back(j);
        }
        Json list = Json::Object();
        list["isIncomplete"] = false;
        list["items"] = arr;
        return list;
    }

    /** @brief Answers `textDocument/hover`. */
    Json HoverAt(const Json &params) const {
        const Request req = ReadRequest(params);
        const MepmlLspHoverInfo info = MepmlLspHover(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        if (!info.found) return Json();
        Json contents = Json::Object();
        contents["kind"] = "plaintext";
        contents["value"] = info.text;
        Json out = Json::Object();
        out["contents"] = contents;
        out["range"] = RangeOnLine(info.line, info.col_start, info.col_end);
        return out;
    }

    /** @brief Answers `textDocument/documentSymbol` with a nested DocumentSymbol tree. */
    Json Symbols(const Json &params) const {
        const std::string uri = params.get("textDocument").get("uri").as_string();
        const std::vector<std::string> &lines = Doc(uri);
        const std::vector<MepmlLspSymbol> syms = MepmlLspSymbols(lines);
        std::vector<Json> nodes(syms.size());
        for (size_t i = syms.size(); i-- > 0;) {
            const MepmlLspSymbol &s = syms[i];
            Json j = Json::Object();
            j["name"] = s.name;
            if (!s.detail.empty()) j["detail"] = s.detail;
            j["kind"] = static_cast<int>(s.kind);
            const int end_col = static_cast<int>(lines[static_cast<size_t>(s.line_end)].size());
            j["range"] = RangeLines(s.line_start, s.line_end, end_col);
            j["selectionRange"] = RangeOnLine(s.sel_line, s.sel_col_start, s.sel_col_end);
            if (!nodes[i].is_null()) j["children"] = nodes[i];
            if (s.parent >= 0 && static_cast<size_t>(s.parent) < nodes.size()) {
                nodes[static_cast<size_t>(s.parent)].push_back(j);
            } else {
                nodes[i] = j;
            }
        }
        Json arr = Json::Array();
        for (size_t i = 0; i < syms.size(); i++)
            if (syms[i].parent < 0 && nodes[i].is_object()) arr.push_back(nodes[i]);
        return arr;
    }

    /** @brief Answers `textDocument/foldingRange`. */
    Json Folds(const Json &params) const {
        const std::string uri = params.get("textDocument").get("uri").as_string();
        Json arr = Json::Array();
        for (const MepmlLspFold &f : MepmlLspFolds(Doc(uri))) {
            Json j = Json::Object();
            j["startLine"] = f.start_line;
            j["endLine"] = f.end_line;
            if (!f.kind.empty()) j["kind"] = f.kind;
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/definition`. */
    Json Definition(const Json &params) const {
        const Request req = ReadRequest(params);
        const MepmlLspLocation loc = MepmlLspDefinition(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        if (!loc.found) return Json();
        Json j = Json::Object();
        j["uri"] = loc.path.empty() ? req.uri : PathToUri(loc.path);
        j["range"] = RangeOnLine(loc.line, loc.col_start, loc.col_end);
        return j;
    }

    /** @brief Answers `textDocument/references`. */
    Json References(const Json &params) const {
        const Request req = ReadRequest(params);
        const MepmlLspReferenceSet refs = MepmlLspReferences(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        const bool include_decl = !params.get("context").is_object() || params.get("context").get("includeDeclaration").as_bool();
        Json arr = Json::Array();
        for (const MepmlLspReference &r : refs.refs) {
            if (r.is_definition && !include_decl) continue;
            Json j = Json::Object();
            j["uri"] = req.uri;
            j["range"] = RangeOnLine(r.line, r.col_start, r.col_end);
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/documentHighlight`. */
    Json Highlights(const Json &params) const {
        const Request req = ReadRequest(params);
        const MepmlLspReferenceSet refs = MepmlLspReferences(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        Json arr = Json::Array();
        for (const MepmlLspReference &r : refs.refs) {
            Json j = Json::Object();
            j["range"] = RangeOnLine(r.line, r.col_start, r.col_end);
            j["kind"] = r.is_definition ? 3 : 2;  // Write (the definition) / Read
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/prepareRename`: the range under the cursor, or null where nothing can be renamed. */
    Json PrepareRename(const Json &params) const {
        const Request req = ReadRequest(params);
        const MepmlLspReferenceSet refs = MepmlLspReferences(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        if (!refs.found || !refs.rename_blocked_reason.empty() || refs.refs.empty()) return Json();
        for (const MepmlLspReference &r : refs.refs)
            if (r.line == req.line && r.col_start <= req.col && req.col <= r.col_end) return RangeOnLine(r.line, r.col_start, r.col_end);
        return RangeOnLine(refs.refs.front().line, refs.refs.front().col_start, refs.refs.front().col_end);
    }

    /** @brief Answers `textDocument/rename` with a single-file WorkspaceEdit. */
    void Rename(const Json &id, const Json &params) const {
        const Request req = ReadRequest(params);
        std::string error;
        const std::vector<MepmlLspTextEdit> edits =
            MepmlLspRename(Doc(req.uri), req.line, req.col, params.get("newName").as_string(), OptionsFor(req.uri), &error);
        if (edits.empty()) {
            ReplyError(id, -32803, error.empty() ? "Nothing to rename here" : error);
            return;
        }
        Json arr = Json::Array();
        for (const MepmlLspTextEdit &e : edits) arr.push_back(EditJson(e));
        Json changes = Json::Object();
        changes[req.uri] = arr;
        Json edit = Json::Object();
        edit["changes"] = changes;
        Reply(id, edit);
    }

    /** @brief Answers `textDocument/codeAction` for the request range's first line. */
    Json CodeActions(const Json &params) const {
        const std::string uri = params.get("textDocument").get("uri").as_string();
        const int line = params.get("range").get("start").get("line").as_int();
        Json arr = Json::Array();
        for (const MepmlLspCodeAction &a : MepmlLspCodeActions(Doc(uri), line, OptionsFor(uri))) {
            Json edits = Json::Array();
            for (const MepmlLspTextEdit &e : a.edits) edits.push_back(EditJson(e));
            Json changes = Json::Object();
            changes[uri] = edits;
            Json wedit = Json::Object();
            wedit["changes"] = changes;
            Json j = Json::Object();
            j["title"] = a.title;
            j["kind"] = a.kind;
            j["edit"] = wedit;
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/formatting` (and rangeFormatting, which formats the whole document: tables only). */
    Json Format(const Json &params) const {
        const std::string uri = params.get("textDocument").get("uri").as_string();
        Json arr = Json::Array();
        for (const MepmlLspTextEdit &e : MepmlLspFormat(Doc(uri))) arr.push_back(EditJson(e));
        return arr;
    }
};

}  // namespace

/**
 * @brief Runs the mepml language server's stdio read loop until the client sends `exit` (or closes the stream).
 * @return the exit status LSP asks for: 0 after a `shutdown`/`exit` handshake, 1 otherwise
 */
int main() {
#if defined(_WIN32)
    // Without this the CRT translates '\n' to "\r\n" on the way out,
    // which corrupts every Content-Length byte count.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    MepmlLanguageServer server;
    std::string buffer;
    char chunk[4096];
    while (server.running()) {
        // A raw read, not fread: fread(buf, 1, N, stdin) blocks until it
        // has all N bytes (or EOF), so with a 4 KiB buffer it would sit
        // on a client's first short `initialize` message forever and the
        // handshake would never complete. A byte-count-framed protocol
        // needs "give me whatever has arrived", which is what a single
        // read() call is.
#if defined(_WIN32)
        const int got = _read(_fileno(stdin), chunk, static_cast<unsigned int>(sizeof(chunk)));
#else
        const ssize_t got = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (got < 0 && errno == EINTR) continue;  // a signal, not end of input
#endif
        if (got <= 0) break;  // the client went away
        buffer.append(chunk, static_cast<size_t>(got));
        if (!PumpRpcFrames(buffer, [&server](const std::string &body) { server.HandleMessage(body); })) break;
    }
    return server.exit_code();
}

// `mep-cpp-lsp`: the wire half of mep's own C++ language server.
// Speaks LSP over stdio; every answer comes from cpp_lsp.h's pure
// analysis functions, which is where the actual C++ knowledge lives
// (cpp_ast.h is the tokenizer, preprocessor and parser under those).
//
// Standalone on purpose, rather than a set of Lua callbacks inside mep,
// for the same three reasons org_lsp_server.cpp gives: it is the shape
// every other server in mep.lsp_servers already has, so kBuiltinLsp
// attaches to it through the same code path as clangd with no special
// case anywhere; a crash or a pathological document costs a subprocess
// rather than the editor; and it works in any LSP client, so a C++ file
// edited outside mep gets the same diagnostics.
//
// Deliberately single-threaded and synchronous. Every request is a parse
// plus a walk of one already-in-memory document -- a few milliseconds for
// files far larger than anyone edits by hand (measured at ~250 ms for
// mep's own 49,000-line main.cpp, and a few milliseconds for a file of
// ordinary size), so there is no work queue, no cancellation handling and
// no background indexing to get wrong. The one thing that is cached is
// the set of headers a document includes, keyed by path and modification
// time (see cpp_lsp.cpp), so a keystroke re-reads none of them.
//
// Transport notes:
//   - Content-Length framing via rpc_framing.h (PumpRpcFrames /
//     FrameRpcMessage), the same helper agent_rpc.cpp uses.
//   - Text synchronization is full (TextDocumentSyncKind.Full = 1):
//     re-analysis is a single linear pass, so incremental updates would
//     buy nothing but a harder-to-get-right document store.
//   - positionEncoding is "utf-8", i.e. positions are byte offsets, to
//     match what mep's client reads `character` as. C++ source is full
//     of non-ASCII string literals and comments, so declaring UTF-16 and
//     not honoring it would put every diagnostic on such a line in the
//     wrong column.
//
// Settings, all optional, under `initializationOptions` (mep passes them
// through `mep.lsp_servers.cpp_ls.init_options`) or
// `workspace/didChangeConfiguration` under a `cpp` key:
//   - `maxLineLength`: columns past which a line earns a Hint. Off by
//     default -- this server has no opinion about line length until a
//     project states one.
//   - `defines`: macros to treat as defined, each "NAME" or "NAME=value".
//     A project's own build-system defines, which decide which `#if`
//     branches this server reads.
//   - `includeDirs`: extra directories a quoted `#include` resolves
//     against, beyond the document's own.

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
#include "cpp_lsp.h"
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

class CppLanguageServer {
public:
    /** @brief Feeds one complete JSON-RPC message body to the server. */
    void HandleMessage(const std::string &body) {
        Json msg;
        if (!Json::Parse(body, &msg)) return;  // a peer that cannot frame JSON is not one we can answer
        const std::string method = msg.get("method").as_string();
        const Json &params = msg.get("params");
        const bool is_request = msg.contains("id");

        if (method == "initialize") {
            ReadSettings(params.get("initializationOptions"));
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
        if (method == "workspace/didChangeConfiguration") {
            if (ReadSettings(params.get("settings").get("cpp"))) {
                for (const auto &doc : docs_) Publish(doc.first);
            }
            return;
        }
        if (method == "initialized" || method == "textDocument/didSave" || method == "$/setTrace") return;
        if (method == "textDocument/didOpen") {
            const std::string uri = params.get("textDocument").get("uri").as_string();
            docs_[uri] = SplitLines(params.get("textDocument").get("text").as_string());
            Publish(uri);
            return;
        }
        if (method == "textDocument/didChange") {
            const std::string uri = params.get("textDocument").get("uri").as_string();
            // Full sync: the last content change carries the whole
            // document (see this file's transport note).
            const Json &changes = params.get("contentChanges");
            if (changes.is_array() && changes.size() > 0) {
                docs_[uri] = SplitLines(changes.items().back().get("text").as_string());
            }
            Publish(uri);
            return;
        }
        if (method == "textDocument/didClose") {
            const std::string uri = params.get("textDocument").get("uri").as_string();
            docs_.erase(uri);
            // Clear the client's diagnostics for a document we no longer
            // track, so stale underlines do not outlive the buffer.
            Json note = Json::Object();
            note["uri"] = uri;
            note["diagnostics"] = Json::Array();
            Notify("textDocument/publishDiagnostics", note);
            return;
        }
        if (!is_request) return;  // an unknown notification is not an error

        if (method == "textDocument/completion") {
            Reply(msg.get("id"), Completion(params));
        } else if (method == "textDocument/hover") {
            Reply(msg.get("id"), HoverAt(params));
        } else if (method == "textDocument/documentSymbol") {
            Reply(msg.get("id"), Symbols(params));
        } else if (method == "textDocument/foldingRange") {
            Reply(msg.get("id"), Folds(params));
        } else if (method == "textDocument/definition" || method == "textDocument/declaration" ||
                   method == "textDocument/typeDefinition" || method == "textDocument/implementation") {
            Reply(msg.get("id"), Definition(params));
        } else if (method == "textDocument/references") {
            Reply(msg.get("id"), References(params));
        } else if (method == "textDocument/documentHighlight") {
            Reply(msg.get("id"), Highlights(params));
        } else if (method == "textDocument/prepareRename") {
            Reply(msg.get("id"), PrepareRename(params));
        } else if (method == "textDocument/rename") {
            Rename(msg.get("id"), params);
        } else if (method == "textDocument/signatureHelp") {
            Reply(msg.get("id"), SignatureHelp(params));
        } else if (method == "completionItem/resolve") {
            // Every item is already complete when it is offered; resolving
            // one is the identity.
            Reply(msg.get("id"), params);
        } else {
            ReplyError(msg.get("id"), -32601, "Unsupported method: " + method);
        }
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
    int max_line_length_ = 0;
    std::vector<std::string> defines_;
    std::vector<std::string> include_dirs_;

    /**
     * @brief Reads this server's settings out of an object, reporting whether any of them changed.
     * @param settings an `initializationOptions` or a `settings.cpp` object; anything else is ignored
     */
    bool ReadSettings(const Json &settings) {
        if (!settings.is_object()) return false;
        bool changed = false;
        if (settings.contains("maxLineLength")) {
            const int value = settings.get("maxLineLength").as_int();
            changed = changed || value != max_line_length_;
            max_line_length_ = value;
        }
        if (settings.contains("defines") && settings.get("defines").is_array()) {
            defines_.clear();
            for (const Json &entry : settings.get("defines").items()) defines_.push_back(entry.as_string());
            changed = true;
        }
        if (settings.contains("includeDirs") && settings.get("includeDirs").is_array()) {
            include_dirs_.clear();
            for (const Json &entry : settings.get("includeDirs").items()) include_dirs_.push_back(entry.as_string());
            changed = true;
        }
        return changed;
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

    /** @brief Builds this server's `initialize` result (capabilities plus server name/version). */
    static Json Capabilities() {
        Json sync = Json::Object();
        sync["openClose"] = true;
        sync["change"] = 1;  // TextDocumentSyncKind.Full

        Json completion = Json::Object();
        completion["resolveProvider"] = false;
        // The characters that open a context a typed prefix would not:
        // a member access either way round, a qualified name, a
        // directive, and the two include-path delimiters.
        Json triggers = Json::Array();
        triggers.push_back(".");
        triggers.push_back(">");
        triggers.push_back(":");
        triggers.push_back("#");
        triggers.push_back("<");
        triggers.push_back("\"");
        completion["triggerCharacters"] = triggers;

        Json signature = Json::Object();
        Json sig_triggers = Json::Array();
        sig_triggers.push_back("(");
        sig_triggers.push_back(",");
        signature["triggerCharacters"] = sig_triggers;

        Json rename = Json::Object();
        rename["prepareProvider"] = true;

        Json caps = Json::Object();
        caps["positionEncoding"] = "utf-8";
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

        Json info = Json::Object();
        info["name"] = "mep-cpp-lsp";
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

    /** @brief Builds the analysis options for a document URI (its directory anchors relative import lookups). */
    CppLspOptions OptionsFor(const std::string &uri) const {
        CppLspOptions opts;
        const std::string path = UriToPath(uri);
        if (!path.empty() && path[0] == '/') {
            const std::filesystem::path fs_path(path);
            opts.doc_dir = fs_path.parent_path().string();
            opts.file_name = fs_path.filename().string();
        }
        // An unsaved/untitled document has no directory to resolve
        // against, and guessing the server's own cwd would resolve
        // includes to files that have nothing to do with it.
        opts.check_files = !opts.doc_dir.empty();
        opts.max_line_length = max_line_length_;
        opts.defines = defines_;
        opts.include_dirs = include_dirs_;
        return opts;
    }

    /** @brief Analyzes a document and pushes the result to the client. */
    void Publish(const std::string &uri) {
        const std::vector<CppLspDiagnostic> diags = CppLspDiagnostics(Doc(uri), OptionsFor(uri));
        Json arr = Json::Array();
        for (const CppLspDiagnostic &d : diags) {
            Json j = Json::Object();
            j["range"] = RangeOnLine(d.line, d.col_start, d.col_end);
            j["severity"] = static_cast<int>(d.severity);
            j["code"] = d.code;
            j["source"] = "cpp";
            j["message"] = d.message;
            arr.push_back(j);
        }
        Json note = Json::Object();
        note["uri"] = uri;
        note["diagnostics"] = arr;
        Notify("textDocument/publishDiagnostics", note);
    }

    /** @brief The (uri, line, character) every position-taking request carries. */
    struct Request {
        std::string uri;
        int line = 0;
        int col = 0;
    };

    /** @brief Reads a TextDocumentPositionParams. */
    static Request ReadRequest(const Json &params) {
        Request r;
        r.uri = params.get("textDocument").get("uri").as_string();
        r.line = params.get("position").get("line").as_int();
        r.col = params.get("position").get("character").as_int();
        return r;
    }

    /** @brief Answers `textDocument/completion`. */
    Json Completion(const Json &params) const {
        const Request req = ReadRequest(params);
        const std::vector<CppLspCompletionItem> items =
            CppLspCompletions(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        Json arr = Json::Array();
        for (size_t i = 0; i < items.size(); i++) {
            const CppLspCompletionItem &it = items[i];
            Json j = Json::Object();
            j["label"] = it.label;
            j["kind"] = static_cast<int>(it.kind);
            j["insertText"] = it.insert_text;
            j["insertTextFormat"] = 1;  // PlainText: never a snippet, see cpp_lsp.h
            if (!it.detail.empty()) j["detail"] = it.detail;
            if (!it.documentation.empty()) j["documentation"] = it.documentation;
            // The order this server produced is the useful one (nearest
            // scope first, then builtins, then keywords); a zero-padded
            // sortText preserves it against a client that would otherwise
            // sort alphabetically.
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
        const CppLspHoverInfo info = CppLspHover(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        if (!info.found) return Json();
        Json contents = Json::Object();
        contents["kind"] = "plaintext";
        contents["value"] = info.text;
        Json out = Json::Object();
        out["contents"] = contents;
        out["range"] = RangeOnLine(info.line, info.col_start, info.col_end);
        return out;
    }

    /** @brief Serializes one symbol and its children, in document order. */
    static Json SymbolNode(const std::vector<CppLspSymbol> &syms, const std::vector<std::vector<int>> &children,
                           const std::vector<std::string> &lines, size_t index) {
        const CppLspSymbol &s = syms[index];
        Json j = Json::Object();
        j["name"] = s.name;
        if (!s.detail.empty()) j["detail"] = s.detail;
        j["kind"] = s.kind;
        const size_t end_line =
            static_cast<size_t>(s.line_end) < lines.size() ? static_cast<size_t>(s.line_end) : lines.size() - 1;
        j["range"] = RangeLines(s.line_start, static_cast<int>(end_line), static_cast<int>(lines[end_line].size()));
        j["selectionRange"] = RangeOnLine(s.sel_line, s.sel_col_start, s.sel_col_end);
        if (!children[index].empty()) {
            Json kids = Json::Array();
            for (int child : children[index]) {
                kids.push_back(SymbolNode(syms, children, lines, static_cast<size_t>(child)));
            }
            j["children"] = kids;
        }
        return j;
    }

    /** @brief Answers `textDocument/documentSymbol` with a nested DocumentSymbol tree. */
    Json Symbols(const Json &params) const {
        const std::string uri = params.get("textDocument").get("uri").as_string();
        const std::vector<std::string> &lines = Doc(uri);
        const std::vector<CppLspSymbol> syms = CppLspSymbols(lines);
        // Children are gathered first, in document order, so the tree
        // comes out reading the way the file does.
        std::vector<std::vector<int>> children(syms.size());
        for (size_t i = 0; i < syms.size(); i++) {
            const int parent = syms[i].parent;
            if (parent >= 0 && static_cast<size_t>(parent) < syms.size()) {
                children[static_cast<size_t>(parent)].push_back(static_cast<int>(i));
            }
        }
        Json arr = Json::Array();
        for (size_t i = 0; i < syms.size(); i++) {
            if (syms[i].parent < 0) arr.push_back(SymbolNode(syms, children, lines, i));
        }
        return arr;
    }

    /** @brief Answers `textDocument/foldingRange`. */
    Json Folds(const Json &params) const {
        const std::string uri = params.get("textDocument").get("uri").as_string();
        Json arr = Json::Array();
        for (const CppLspFold &f : CppLspFolds(Doc(uri))) {
            Json j = Json::Object();
            j["startLine"] = f.start_line;
            j["endLine"] = f.end_line;
            if (!f.kind.empty()) j["kind"] = f.kind;
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/definition` (and the declaration/type/implementation aliases). */
    Json Definition(const Json &params) const {
        const Request req = ReadRequest(params);
        const CppLspLocation loc = CppLspDefinition(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        if (!loc.found) return Json();
        Json j = Json::Object();
        j["uri"] = loc.path.empty() ? req.uri : PathToUri(loc.path);
        j["range"] = RangeOnLine(loc.line, loc.col, loc.col);
        return j;
    }

    /** @brief Answers `textDocument/references`. */
    Json References(const Json &params) const {
        const Request req = ReadRequest(params);
        const CppLspReferenceSet refs = CppLspReferences(Doc(req.uri), req.line, req.col);
        const bool include_decl = !params.get("context").is_object() ||
                                  params.get("context").get("includeDeclaration").as_bool();
        Json arr = Json::Array();
        for (const CppLspReference &r : refs.refs) {
            if (r.is_write && !include_decl) continue;
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
        const CppLspReferenceSet refs = CppLspReferences(Doc(req.uri), req.line, req.col);
        Json arr = Json::Array();
        for (const CppLspReference &r : refs.refs) {
            Json j = Json::Object();
            j["range"] = RangeOnLine(r.line, r.col_start, r.col_end);
            j["kind"] = r.is_write ? 3 : 2;  // DocumentHighlightKind.Write / .Read
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/prepareRename`, refusing the names this server may not rename. */
    Json PrepareRename(const Json &params) const {
        const Request req = ReadRequest(params);
        const CppLspReferenceSet refs = CppLspReferences(Doc(req.uri), req.line, req.col);
        if (!refs.found || !refs.rename_blocked_reason.empty() || refs.refs.empty()) return Json();
        return RangeOnLine(refs.refs.front().line, refs.refs.front().col_start, refs.refs.front().col_end);
    }

    /** @brief Answers `textDocument/rename` with a single-file WorkspaceEdit. */
    void Rename(const Json &id, const Json &params) const {
        const Request req = ReadRequest(params);
        const std::string new_name = params.get("newName").as_string();
        const CppLspReferenceSet refs = CppLspReferences(Doc(req.uri), req.line, req.col);
        if (!refs.found || refs.refs.empty()) {
            ReplyError(id, -32803, "Nothing to rename here");
            return;
        }
        if (!refs.rename_blocked_reason.empty()) {
            ReplyError(id, -32803, "Cannot rename: " + refs.rename_blocked_reason);
            return;
        }
        Json edits = Json::Array();
        for (const CppLspReference &r : refs.refs) {
            Json e = Json::Object();
            e["range"] = RangeOnLine(r.line, r.col_start, r.col_end);
            e["newText"] = new_name;
            edits.push_back(e);
        }
        Json changes = Json::Object();
        changes[req.uri] = edits;
        Json edit = Json::Object();
        edit["changes"] = changes;
        Reply(id, edit);
    }

    /** @brief Answers `textDocument/signatureHelp`. */
    Json SignatureHelp(const Json &params) const {
        const Request req = ReadRequest(params);
        const CppLspSignature sig = CppLspSignatureHelp(Doc(req.uri), req.line, req.col);
        if (!sig.found) return Json();
        Json parameters = Json::Array();
        for (const std::string &p : sig.params) {
            Json j = Json::Object();
            j["label"] = p;
            parameters.push_back(j);
        }
        Json signature = Json::Object();
        signature["label"] = sig.label;
        if (!sig.documentation.empty()) signature["documentation"] = sig.documentation;
        signature["parameters"] = parameters;
        Json signatures = Json::Array();
        signatures.push_back(signature);
        Json out = Json::Object();
        out["signatures"] = signatures;
        out["activeSignature"] = 0;
        out["activeParameter"] = sig.active_param;
        return out;
    }
};

}  // namespace

/**
 * @brief Runs the C++ language server's stdio read loop until the client sends `exit` (or closes the stream).
 * @return the exit status LSP asks for: 0 after a `shutdown`/`exit` handshake, 1 otherwise
 */
int main() {
#if defined(_WIN32)
    // Without this the CRT translates '\n' to "\r\n" on the way out,
    // which corrupts every Content-Length byte count.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    CppLanguageServer server;
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

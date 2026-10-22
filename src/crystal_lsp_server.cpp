// `mep-crystal-lsp`: the wire half of mep's own Crystal language server,
// and a command-line linter. Speaks LSP over stdio; every answer comes from
// crystal_lsp.h's analysis over mep's port of the Crystal compiler's own
// parser (crystal_parser.h).
//
// Standalone, like every entry in mep.lsp_servers. It never runs the
// compiler: the syntax errors it reports are the compiler's, word for word,
// and the rest is read off the tree and the standard library's API table.
//
// On `initialize` it indexes the workspace's .cr files -- the types,
// methods, macros and constants each one defines, with their signatures and
// doc comments, `lib/` shards included -- so definition, hover, completion
// and signature help reach across files; an open document is re-indexed as
// it changes.
//
// `mep-crystal-lsp --lint FILE...` prints every diagnostic as
// `path:line:col: severity: message [code]` (1-based) and exits 1 when any
// of them is an error or a warning, 0 otherwise; `--lint -` reads standard
// input. With no arguments it is the language server.
//
// Single-threaded and synchronous; Content-Length framing via
// rpc_framing.h, full text sync, `positionEncoding: "utf-8"`.
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include "json.h"
#include "crystal_lsp.h"
#include "rpc_framing.h"

namespace {

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

/** @brief Builds an LSP Range object from an analysis range. */
Json RangeOf(const CrRange &range) {
    Json r = Json::Object();
    r["start"] = Position(range.start_line, range.start_col);
    r["end"] = Position(range.end_line, range.end_col);
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

class CrystalLanguageServer {
public:
    /** @brief Feeds one complete JSON-RPC message body to the server. */
    void HandleMessage(const std::string &body) {
        Json msg;
        if (!Json::Parse(body, &msg)) return;  // a peer that cannot frame JSON is not one we can answer
        const std::string method = msg.get("method").as_string();
        const Json &params = msg.get("params");
        const bool is_request = msg.contains("id");

        if (method == "initialize") {
            Reply(msg.get("id"), Capabilities());
            std::string root = params.get("rootUri").as_string();
            if (root.empty()) root = params.get("rootPath").as_string();
            if (!root.empty()) Index(PathOf(root));
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
        if (method == "initialized" || method == "workspace/didChangeConfiguration" || method == "$/setTrace") {
            return;
        }
        if (method == "textDocument/didSave") {
            const std::string uri = params.get("textDocument").get("uri").as_string();
            CrProjectAddFile(&project_, PathOf(uri), Doc(uri));
            return;
        }
        if (method == "textDocument/didOpen") {
            const std::string uri = params.get("textDocument").get("uri").as_string();
            docs_[uri] = SplitLines(params.get("textDocument").get("text").as_string());
            CrProjectAddFile(&project_, PathOf(uri), docs_[uri]);
            Publish(uri);
            return;
        }
        if (method == "textDocument/didChange") {
            const std::string uri = params.get("textDocument").get("uri").as_string();
            // Full sync: the last content change carries the whole document.
            const Json &changes = params.get("contentChanges");
            if (changes.is_array() && changes.size() > 0) {
                docs_[uri] = SplitLines(changes.items().back().get("text").as_string());
                CrProjectAddFile(&project_, PathOf(uri), docs_[uri]);
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
                ReplyError(msg.get("id"), -32803, error);  // RequestFailed
            } else {
                Reply(msg.get("id"), result);
            }
        } else if (method == "textDocument/formatting") {
            Reply(msg.get("id"), Formatting(params));
        } else if (method == "completionItem/resolve") {
            // Every item is already complete when it is offered.
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
    CrProject project_;

    /** @brief A file:// URI's path (percent-escapes decoded); anything else as it is. */
    static std::string PathOf(const std::string &uri) {
        if (uri.compare(0, 7, "file://") != 0) return uri;
        std::string out;
        for (size_t k = 7; k < uri.size(); k++) {
            if (uri[k] == '%' && k + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[k + 1])) &&
                std::isxdigit(static_cast<unsigned char>(uri[k + 2]))) {
                out += static_cast<char>(std::stoi(uri.substr(k + 1, 2), nullptr, 16));
                k += 2;
            } else {
                out += uri[k];
            }
        }
        return out;
    }

    static std::string UriFor(const std::string &path) { return "file://" + path; }

    /** @brief Indexes the workspace's Crystal files (skipping build output and hidden directories). */
    void Index(const std::string &root) {
        size_t budget = 20000;
        std::vector<std::string> stack = {root};
        while (!stack.empty() && budget > 0) {
            const std::string dir = stack.back();
            stack.pop_back();
            DIR *d = opendir(dir.c_str());
            if (!d) continue;
            while (dirent *e = readdir(d)) {
                const std::string name = e->d_name;
                if (name.empty() || name[0] == '.' || name == "node_modules" || name == "bin" || name == "docs") continue;
                const std::string path = dir + "/" + name;
                struct stat st;
                if (stat(path.c_str(), &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    stack.push_back(path);
                    continue;
                }
                const size_t dot = name.find_last_of('.');
                const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
                if (ext != "cr") continue;
                if (st.st_size > 4 * 1024 * 1024 || budget == 0) continue;
                budget--;
                std::ifstream in(path, std::ios::binary);
                std::ostringstream ss;
                ss << in.rdbuf();
                CrProjectAddFile(&project_, path, SplitLines(ss.str()));
            }
            closedir(d);
        }
    }
    bool running_ = true;
    bool shutting_down_ = false;
    int exit_code_ = 1;

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
        // `.` a receiver's methods, `::` a namespace's contents, `@` instance variables.
        Json completion_triggers = Json::Array();
        for (const char *t : {".", ":", "@"}) completion_triggers.push_back(t);
        completion["triggerCharacters"] = completion_triggers;

        Json signature = Json::Object();
        Json signature_triggers = Json::Array();
        // `(` opens a call; `,` moves the active argument along.
        signature_triggers.push_back("(");
        signature_triggers.push_back(",");
        signature["triggerCharacters"] = signature_triggers;

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
        caps["documentFormattingProvider"] = true;

        Json info = Json::Object();
        info["name"] = "mep-crystal-lsp";
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

    /** @brief The document URI a request names. */
    static std::string UriOf(const Json &params) { return params.get("textDocument").get("uri").as_string(); }
    /** @brief The 0-based line a request's position names. */
    static int LineOf(const Json &params) { return params.get("position").get("line").as_int(); }
    /** @brief The 0-based byte column a request's position names. */
    static int ColOf(const Json &params) { return params.get("position").get("character").as_int(); }

    /** @brief Lints a document and pushes the result to the client. */
    void Publish(const std::string &uri) {
        Json arr = Json::Array();
        for (const CrLspDiagnostic &d : CrLspDiagnostics(Doc(uri), PathOf(uri))) {
            Json j = Json::Object();
            j["range"] = RangeOf(d.range);
            j["severity"] = static_cast<int>(d.severity);
            j["code"] = d.code;
            j["source"] = "crystal";
            j["message"] = d.message;
            if (d.unnecessary) {
                Json tags = Json::Array();
                tags.push_back(1);  // DiagnosticTag.Unnecessary
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
        const int line = LineOf(params);
        const std::string uri = UriOf(params);
        const std::vector<CrLspCompletionItem> items =
            CrLspCompletions(Doc(uri), line, ColOf(params), &project_);
        Json arr = Json::Array();
        for (size_t i = 0; i < items.size(); i++) {
            const CrLspCompletionItem &it = items[i];
            Json j = Json::Object();
            j["label"] = it.label;
            j["kind"] = static_cast<int>(it.kind);
            j["insertText"] = it.insert_text;
            j["insertTextFormat"] = 1;  // PlainText: never a snippet
            if (!it.detail.empty()) j["detail"] = it.detail;
            if (!it.documentation.empty()) j["documentation"] = it.documentation;
            // Locals first, then the file's, the workspace's, the stdlib's.
            j["sortText"] = it.sort_text;
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

    /** @brief Answers `textDocument/hover`. */
    Json HoverAt(const Json &params) const {
        const std::string uri = UriOf(params);
        const CrLspHoverInfo info = CrLspHover(Doc(uri), LineOf(params), ColOf(params), &project_);
        if (!info.found) return Json();
        Json contents = Json::Object();
        contents["kind"] = "markdown";
        contents["value"] = info.text;
        Json out = Json::Object();
        out["contents"] = contents;
        out["range"] = RangeOf(info.range);
        return out;
    }

    /** @brief Answers `textDocument/documentSymbol` with a nested DocumentSymbol tree. */
    Json Symbols(const Json &params) const {
        const std::vector<std::string> &lines = Doc(UriOf(params));
        const std::vector<CrLspSymbol> syms = CrLspSymbols(lines);
        // Built leaf-last so a parent's `children` array is complete by
        // the time the parent itself is serialized.
        std::vector<Json> nodes(syms.size());
        for (size_t i = syms.size(); i-- > 0;) {
            const CrLspSymbol &s = syms[i];
            Json j = Json::Object();
            j["name"] = s.name;
            if (!s.detail.empty()) j["detail"] = s.detail;
            j["kind"] = static_cast<int>(s.kind);
            const size_t end_line = static_cast<size_t>(s.range.end_line) < lines.size()
                                        ? static_cast<size_t>(s.range.end_line)
                                        : lines.size() - 1;
            j["range"] = RangeLines(s.range.start_line, static_cast<int>(end_line),
                                    static_cast<int>(lines[end_line].size()));
            j["selectionRange"] = RangeOf(s.sel_range);
            if (!nodes[i].is_null()) j["children"] = nodes[i];
            if (s.parent >= 0 && static_cast<size_t>(s.parent) < nodes.size()) {
                nodes[static_cast<size_t>(s.parent)].push_back(j);
            } else {
                nodes[i] = j;  // a root: hold it here until the final pass
            }
        }
        Json arr = Json::Array();
        for (size_t i = 0; i < syms.size(); i++) {
            if (syms[i].parent < 0 && nodes[i].is_object()) arr.push_back(nodes[i]);
        }
        return arr;
    }

    /** @brief Answers `textDocument/foldingRange`. */
    Json Folds(const Json &params) const {
        Json arr = Json::Array();
        for (const CrLspFold &f : CrLspFolds(Doc(UriOf(params)))) {
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
        const std::string uri = UriOf(params);
        const CrLspLocation loc = CrLspDefinition(Doc(uri), LineOf(params), ColOf(params), &project_);
        if (!loc.found) return Json();
        Json j = Json::Object();
        j["uri"] = loc.path.empty() ? uri : UriFor(loc.path);
        j["range"] = RangeOf(loc.range);
        return j;
    }

    /** @brief Answers `textDocument/references`. */
    Json References(const Json &params) const {
        const std::string uri = UriOf(params);
        // LSP defaults to including the declaration; only an explicit
        // `includeDeclaration: false` drops it.
        const bool include_declaration =
            !params.contains("context") || params.get("context").get("includeDeclaration").as_bool();
        Json arr = Json::Array();
        for (const CrLspReference &ref : CrLspReferences(Doc(uri), LineOf(params), ColOf(params))) {
            if (ref.is_definition && !include_declaration) continue;
            Json j = Json::Object();
            j["uri"] = uri;
            j["range"] = RangeOf(ref.range);
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/documentHighlight`, reusing the same occurrence list as references. */
    Json Highlights(const Json &params) const {
        Json arr = Json::Array();
        const std::string uri = UriOf(params);
        for (const CrLspReference &ref : CrLspReferences(Doc(uri), LineOf(params), ColOf(params))) {
            Json j = Json::Object();
            j["range"] = RangeOf(ref.range);
            j["kind"] = ref.is_definition || ref.is_write ? 3 : 2;  // DocumentHighlightKind.Write / .Read
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/prepareRename`. */
    Json PrepareRename(const Json &params) const {
        const std::string uri = UriOf(params);
        const CrLspRenameTarget t = CrLspPrepareRename(Doc(uri), LineOf(params), ColOf(params));
        if (!t.found) return Json();
        Json out = Json::Object();
        out["range"] = RangeOf(t.range);
        out["placeholder"] = t.placeholder;
        return out;
    }

    /** @brief Answers `textDocument/rename` with a WorkspaceEdit over this one document. */
    Json Rename(const Json &params, std::string *error) const {
        const std::string uri = UriOf(params);
        const std::vector<CrLspTextEdit> edits = CrLspRename(
            Doc(uri), LineOf(params), ColOf(params), params.get("newName").as_string(), error);
        if (!error->empty()) return Json();
        Json arr = Json::Array();
        for (const CrLspTextEdit &e : edits) {
            Json j = Json::Object();
            j["range"] = RangeOf(e.range);
            j["newText"] = e.new_text;
            arr.push_back(j);
        }
        Json changes = Json::Object();
        changes[uri] = arr;
        Json out = Json::Object();
        out["changes"] = changes;
        return out;
    }

    /** @brief Answers `textDocument/formatting` with mep's Crystal formatter (`crystal tool format`'s
     *         layout): one edit over the lines that changed, none when the file does not parse. */
    Json Formatting(const Json &params) const {
        Json arr = Json::Array();
        for (const CrLspTextEdit &e : CrLspFormat(Doc(UriOf(params)))) {
            Json j = Json::Object();
            j["range"] = RangeOf(e.range);
            j["newText"] = e.new_text;
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/signatureHelp`: one signature per arity, the matching one active. */
    Json SignatureHelp(const Json &params) const {
        const std::string uri = UriOf(params);
        const CrLspSignatureHelp help =
            CrLspSignatures(Doc(uri), LineOf(params), ColOf(params), &project_);
        if (!help.found) return Json();
        Json signatures = Json::Array();
        for (const CrLspSignature &sig : help.signatures) {
            Json parameters = Json::Array();
            for (const std::string &param : sig.params) {
                Json p = Json::Object();
                p["label"] = param;
                parameters.push_back(p);
            }
            Json signature = Json::Object();
            signature["label"] = sig.label;
            if (!sig.documentation.empty()) signature["documentation"] = sig.documentation;
            signature["parameters"] = parameters;
            signatures.push_back(signature);
        }
        Json out = Json::Object();
        out["signatures"] = signatures;
        out["activeSignature"] = help.active_signature;
        if (help.active_param >= 0) out["activeParameter"] = help.active_param;
        return out;
    }
};

/** @brief Runs `--lint` over each path (`-` for stdin), printing diagnostics; returns the process exit status. */
int Lint(const std::vector<std::string> &paths) {
    bool failed = false;
    bool unreadable = false;
    for (const std::string &path : paths) {
        std::string text;
        if (path == "-") {
            text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        } else {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "mep-crystal-lsp: cannot read %s\n", path.c_str());
                unreadable = true;
                continue;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        for (const CrLspDiagnostic &d : CrLspDiagnostics(SplitLines(text), path == "-" ? "" : path)) {
            std::printf("%s\n", CrLspFormatDiagnostic(path, d).c_str());
            if (CrLspDiagnosticFails(d)) failed = true;
        }
    }
    if (unreadable) return 2;
    return failed ? 1 : 0;
}

}  // namespace

/**
 * @brief Runs the Crystal language server's stdio loop, or the `--lint` command-line linter.
 * @return for the server, 0 after a `shutdown`/`exit` handshake and 1 otherwise; for `--lint`, 1 when any
 *         error or warning was printed, 2 when a file could not be read, 0 otherwise
 */
int main(int argc, char **argv) {
#if defined(_WIN32)
    // Without this the CRT translates '\n' to "\r\n" on the way out,
    // which corrupts every Content-Length byte count.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (argc > 1) {
        const std::string first = argv[1];
        if (first == "--lint") {
            std::vector<std::string> paths;
            for (int i = 2; i < argc; i++) paths.emplace_back(argv[i]);
            if (paths.empty()) paths.emplace_back("-");
            return Lint(paths);
        }
        if (first == "--help" || first == "-h") {
            std::printf("usage: mep-crystal-lsp                 run the language server on stdio\n"
                        "       mep-crystal-lsp --lint FILE...  print diagnostics (FILE '-' reads stdin)\n");
            return 0;
        }
        if (first != "--stdio") {
            std::fprintf(stderr, "mep-crystal-lsp: unknown argument %s (try --help)\n", first.c_str());
            return 2;
        }
    }
    CrystalLanguageServer server;
    std::string buffer;
    char chunk[4096];
    while (server.running()) {
        // A raw read, not fread: fread blocks until it has the whole
        // buffer (or EOF), so it would sit on a client's first short
        // `initialize` message forever.
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

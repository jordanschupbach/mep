// `mep-lisp-lsp`: the wire half of mep's own Common Lisp language server,
// and a command-line linter. Speaks LSP over stdio; every answer comes
// from lisp_lsp.h's pure analysis functions, which is where the actual
// Lisp knowledge lives.
//
// Standalone on purpose, rather than a set of Lua callbacks inside mep,
// for the reasons org_lsp_server.cpp gives: it is the shape every other
// entry in mep.lsp_servers already has, a pathological document costs a
// subprocess rather than the editor, and a `.lisp` file edited in any
// other LSP client gets the same diagnostics.
//
// Two things make this server unusual among Common Lisp tooling, and both
// are deliberate:
//   - It never starts a Lisp. SLIME, Sly and the alive/cl-lsp servers all
//     talk to a running image, which is the right design for an
//     interactive session and the wrong one for "open a file and see
//     what is wrong with it": no image to boot, no system to load, no
//     Quicklisp, and an answer on a machine with no Lisp installed.
//   - It never evaluates the document. Reading Lisp in a real image runs
//     `#.` forms and loading it runs everything; this reads the text
//     with its own reader and nothing in the file can execute.
//
// `mep-lisp-lsp --lint FILE...` prints every diagnostic as
// `path:line:col: severity: message [code]` (1-based) and exits 1 when
// any of them is an error or a warning, 0 otherwise; `--lint -` reads
// standard input. With no arguments it is the language server.
//
// Deliberately single-threaded and synchronous: every request is a read
// plus a walk over one in-memory document. Transport is Content-Length
// framing via rpc_framing.h, full text sync, and `positionEncoding:
// "utf-8"` -- byte columns, which is what mep's own client reads and what
// the analysis half produces.
#include <cctype>
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
#include "lisp_lsp.h"
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
Json RangeOf(const LispRange &range) {
    Json r = Json::Object();
    r["start"] = Position(range.line, range.col);
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

class LispLanguageServer {
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
        if (method == "initialized" || method == "workspace/didChangeConfiguration" ||
            method == "textDocument/didSave" || method == "$/setTrace") {
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
            // Full sync: the last content change carries the whole document.
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
        // `:` opens a keyword argument, `(` an operator position, `&` a
        // lambda-list keyword. A client that only fires completion on
        // word characters still works: every candidate also matches a
        // plain typed prefix.
        Json completion_triggers = Json::Array();
        for (const char *t : {":", "(", "&"}) completion_triggers.push_back(t);
        completion["triggerCharacters"] = completion_triggers;

        Json signature = Json::Object();
        Json signature_triggers = Json::Array();
        // Lisp separates arguments with spaces, so a space is what moves
        // the active parameter along.
        signature_triggers.push_back("(");
        signature_triggers.push_back(" ");
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
        info["name"] = "mep-lisp-lsp";
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
        for (const LispLspDiagnostic &d : LispLspDiagnostics(Doc(uri))) {
            Json j = Json::Object();
            j["range"] = RangeOf(d.range);
            j["severity"] = static_cast<int>(d.severity);
            j["code"] = d.code;
            j["source"] = "lisp";
            j["message"] = d.message;
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
        const std::vector<LispLspCompletionItem> items = LispLspCompletions(Doc(UriOf(params)), line, ColOf(params));
        Json arr = Json::Array();
        for (size_t i = 0; i < items.size(); i++) {
            const LispLspCompletionItem &it = items[i];
            Json j = Json::Object();
            j["label"] = it.label;
            j["kind"] = static_cast<int>(it.kind);
            j["insertText"] = it.insert_text;
            j["insertTextFormat"] = 1;  // PlainText: never a snippet
            if (!it.detail.empty()) j["detail"] = it.detail;
            if (!it.documentation.empty()) j["documentation"] = it.documentation;
            // The order this server produced is the useful one (locals,
            // then the file, then COMMON-LISP); a zero-padded sortText
            // preserves it against a client that would sort alphabetically.
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

    /** @brief Answers `textDocument/hover`. */
    Json HoverAt(const Json &params) const {
        const LispLspHoverInfo info = LispLspHover(Doc(UriOf(params)), LineOf(params), ColOf(params));
        if (!info.found) return Json();
        Json contents = Json::Object();
        contents["kind"] = "plaintext";
        contents["value"] = info.text;
        Json out = Json::Object();
        out["contents"] = contents;
        out["range"] = RangeOf(info.range);
        return out;
    }

    /** @brief Answers `textDocument/documentSymbol` with a nested DocumentSymbol tree. */
    Json Symbols(const Json &params) const {
        const std::vector<std::string> &lines = Doc(UriOf(params));
        const std::vector<LispLspSymbol> syms = LispLspSymbols(lines);
        // Built leaf-last so a parent's `children` array is complete by
        // the time the parent itself is serialized.
        std::vector<Json> nodes(syms.size());
        for (size_t i = syms.size(); i-- > 0;) {
            const LispLspSymbol &s = syms[i];
            Json j = Json::Object();
            j["name"] = s.name;
            if (!s.detail.empty()) j["detail"] = s.detail;
            j["kind"] = static_cast<int>(s.kind);
            const size_t end_line = static_cast<size_t>(s.range.end_line) < lines.size()
                                        ? static_cast<size_t>(s.range.end_line)
                                        : lines.size() - 1;
            j["range"] = RangeLines(s.range.line, static_cast<int>(end_line),
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
        for (const LispLspFold &f : LispLspFolds(Doc(UriOf(params)))) {
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
        const LispLspLocation loc = LispLspDefinition(Doc(uri), LineOf(params), ColOf(params));
        if (!loc.found) return Json();
        Json j = Json::Object();
        j["uri"] = uri;
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
        for (const LispLspReference &ref : LispLspReferences(Doc(uri), LineOf(params), ColOf(params))) {
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
        for (const LispLspReference &ref : LispLspReferences(Doc(UriOf(params)), LineOf(params), ColOf(params))) {
            Json j = Json::Object();
            j["range"] = RangeOf(ref.range);
            j["kind"] = ref.is_write ? 3 : 2;  // DocumentHighlightKind.Write / .Read
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/prepareRename`. */
    Json PrepareRename(const Json &params) const {
        const LispLspRenameTarget t = LispLspPrepareRename(Doc(UriOf(params)), LineOf(params), ColOf(params));
        if (!t.found) return Json();
        Json out = Json::Object();
        out["range"] = RangeOf(t.range);
        out["placeholder"] = t.placeholder;
        return out;
    }

    /** @brief Answers `textDocument/rename` with a WorkspaceEdit over this one document. */
    Json Rename(const Json &params, std::string *error) const {
        const std::string uri = UriOf(params);
        const std::vector<LispLspTextEdit> edits = LispLspRename(
            Doc(uri), LineOf(params), ColOf(params), params.get("newName").as_string(), error);
        if (!error->empty()) return Json();
        Json arr = Json::Array();
        for (const LispLspTextEdit &e : edits) {
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

    /** @brief Answers `textDocument/formatting`: mep's Common Lisp formatter, as one edit. */
    Json Formatting(const Json &params) const {
        Json arr = Json::Array();
        for (const LispLspTextEdit &e : LispLspFormat(Doc(UriOf(params)))) {
            Json j = Json::Object();
            j["range"] = RangeOf(e.range);
            j["newText"] = e.new_text;
            arr.push_back(j);
        }
        return arr;
    }

    /** @brief Answers `textDocument/signatureHelp`. */
    Json SignatureHelp(const Json &params) const {
        const LispLspSignature sig = LispLspSignatureHelp(Doc(UriOf(params)), LineOf(params), ColOf(params));
        if (!sig.found) return Json();
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
        if (sig.active_param >= 0) signature["activeParameter"] = sig.active_param;
        Json signatures = Json::Array();
        signatures.push_back(signature);
        Json out = Json::Object();
        out["signatures"] = signatures;
        out["activeSignature"] = 0;
        if (sig.active_param >= 0) out["activeParameter"] = sig.active_param;
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
                std::fprintf(stderr, "mep-lisp-lsp: cannot read %s\n", path.c_str());
                unreadable = true;
                continue;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        for (const LispLspDiagnostic &d : LispLspDiagnostics(SplitLines(text))) {
            std::printf("%s\n", LispLspFormatDiagnostic(path, d).c_str());
            if (LispLspDiagnosticFails(d)) failed = true;
        }
    }
    if (unreadable) return 2;
    return failed ? 1 : 0;
}

}  // namespace

/**
 * @brief Runs the Common Lisp language server's stdio loop, or the `--lint` command-line linter.
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
            std::printf("usage: mep-lisp-lsp                 run the language server on stdio\n"
                        "       mep-lisp-lsp --lint FILE...  print diagnostics (FILE '-' reads stdin)\n");
            return 0;
        }
        if (first != "--stdio") {
            std::fprintf(stderr, "mep-lisp-lsp: unknown argument %s (try --help)\n", first.c_str());
            return 2;
        }
    }
    LispLanguageServer server;
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

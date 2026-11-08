// `mep-nix-lsp`: the wire half of mep's own Nix language server, and a
// command-line linter. Speaks LSP over stdio; every answer comes from
// nix_lsp.h's pure analysis functions over nix_parse.h's parser.
//
// Builtin documentation is Nix's own: the first hover or completion that
// wants it runs `nix __dump-language` once and keeps the result. Without a
// `nix` on PATH everything still works, the docs are just absent.
//
//   mep-nix-lsp                         the language server on stdio
//   mep-nix-lsp --lint FILE...          diagnostics as path:line:col: severity: message [code];
//                                       exit 1 on any error or warning ('-' reads stdin)
//   mep-nix-lsp --nix-check FILE...     what `nix-instantiate --parse` would report, for
//                                       tools/nix_parse_check.py: one line per file,
//                                       `ok`, `parse\tOFFSET\tMESSAGE` or `undefined\tOFFSET\tNAME...`
//
// Single-threaded and synchronous; Content-Length framing via
// rpc_framing.h, full text sync, `positionEncoding: "utf-8"`.
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
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "json.h"
#include "nix_lsp.h"
#include "rpc_framing.h"

namespace {

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

/** @brief Runs argv, returning its standard output and exit status (-1 when it could not start). */
std::string RunCapture(const std::vector<std::string> &argv, int *status) {
    *status = -1;
#if defined(_WIN32)
    (void)argv;
    return std::string();
#else
    int out[2];
    if (pipe(out) != 0) return std::string();
    const pid_t pid = fork();
    if (pid < 0) {
        close(out[0]);
        close(out[1]);
        return std::string();
    }
    if (pid == 0) {
        dup2(out[1], STDOUT_FILENO);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        close(out[0]);
        close(out[1]);
        std::vector<char *> args;
        for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(out[1]);
    std::string data;
    char buf[8192];
    while (true) {
        const ssize_t n = read(out[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
    }
    close(out[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return data;
#endif
}

/** @brief Loads the builtin docs from the installed Nix, once. */
void EnsureBuiltinDocs() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    int status = -1;
    // A hidden command: `nix --extra-experimental-features ... __dump-language` does not find it.
    const std::string out = RunCapture({"nix", "__dump-language"}, &status);
    Json j;
    if (status != 0 || !Json::Parse(out, &j) || !j.is_object()) return;
    // One key per builtin (`builtins` itself among them), each with its "doc".
    std::map<std::string, std::string> docs;
    for (const auto &kv : j.fields()) {
        const std::string doc = kv.second.get("doc").as_string(std::string());
        if (!doc.empty()) docs[kv.first] = doc;
    }
    NixLspSetBuiltinDocs(std::move(docs));
}

// --- JSON shorthands --------------------------------------------------

Json Position(int line, int character) {
    Json p = Json::Object();
    p["line"] = line;
    p["character"] = character;
    return p;
}

Json RangeOf(const NixLspRange &range) {
    Json r = Json::Object();
    r["start"] = Position(range.line, range.col);
    r["end"] = Position(range.end_line, range.end_col);
    return r;
}

Json EditsJson(const std::vector<NixLspTextEdit> &edits) {
    Json arr = Json::Array();
    for (const NixLspTextEdit &e : edits) {
        Json j = Json::Object();
        j["range"] = RangeOf(e.range);
        j["newText"] = e.new_text;
        arr.push_back(j);
    }
    return arr;
}

// --- Server -----------------------------------------------------------

class NixLanguageServer {
public:
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
            // rangeFormatting formats the whole document: nixfmt's layout depends on all of it.
            std::string error;
            const std::vector<NixLspTextEdit> edits = NixLspFormat(Doc(UriOf(params)).lines, 100, &error);
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
        std::shared_ptr<const NixLspDocument> analysis;
    };
    std::map<std::string, OpenDoc> docs_;
    bool running_ = true;
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

    static Json Capabilities() {
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
        caps["documentRangeFormattingProvider"] = true;

        Json info = Json::Object();
        info["name"] = "mep-nix-lsp";
        info["version"] = "1";

        Json result = Json::Object();
        result["capabilities"] = caps;
        result["serverInfo"] = info;
        return result;
    }

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

    void Set(const std::string &uri, const std::string &text) {
        OpenDoc &d = docs_[uri];
        d.lines = SplitLines(text);
        NixLspOptions opts;
        opts.origin = PathOf(uri);
        d.analysis = NixLspAnalyze(d.lines, opts);
    }

    const OpenDoc &Doc(const std::string &uri) {
        const auto it = docs_.find(uri);
        if (it != docs_.end()) return it->second;
        Set(uri, std::string());
        return docs_[uri];
    }
    const NixLspDocument &Analysis(const Json &params) { return *Doc(UriOf(params)).analysis; }

    static std::string UriOf(const Json &params) { return params.get("textDocument").get("uri").as_string(); }
    static int LineOf(const Json &params) { return params.get("position").get("line").as_int(); }
    static int ColOf(const Json &params) { return params.get("position").get("character").as_int(); }

    void Publish(const std::string &uri) {
        Json arr = Json::Array();
        for (const NixLspDiagnostic &d : NixLspDiagnostics(*Doc(uri).analysis)) {
            Json j = Json::Object();
            j["range"] = RangeOf(d.range);
            j["severity"] = static_cast<int>(d.severity);
            j["code"] = d.code;
            j["source"] = "nix";
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
        EnsureBuiltinDocs();
        const int line = LineOf(params);
        const std::vector<NixLspCompletionItem> items = NixLspCompletions(Analysis(params), line, ColOf(params));
        Json arr = Json::Array();
        for (size_t i = 0; i < items.size(); i++) {
            const NixLspCompletionItem &it = items[i];
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
        EnsureBuiltinDocs();
        const NixLspHoverInfo info = NixLspHover(Analysis(params), LineOf(params), ColOf(params));
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
        const std::vector<NixLspSymbol> syms = NixLspSymbols(Analysis(params));
        std::vector<Json> nodes(syms.size());
        for (size_t i = syms.size(); i-- > 0;) {
            const NixLspSymbol &s = syms[i];
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
        for (const NixLspFold &f : NixLspFolds(Analysis(params))) {
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
        for (const NixLspLocation &loc : NixLspDefinition(Analysis(params), LineOf(params), ColOf(params))) {
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
        for (const NixLspReference &ref : NixLspReferences(Analysis(params), LineOf(params), ColOf(params))) {
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
        for (const NixLspReference &ref : NixLspReferences(Analysis(params), LineOf(params), ColOf(params))) {
            Json j = Json::Object();
            j["range"] = RangeOf(ref.range);
            j["kind"] = ref.is_definition ? 3 : 2;
            arr.push_back(j);
        }
        return arr;
    }

    Json PrepareRename(const Json &params) {
        const NixLspRenameTarget t = NixLspPrepareRename(Analysis(params), LineOf(params), ColOf(params));
        if (!t.found) return Json();
        Json out = Json::Object();
        out["range"] = RangeOf(t.range);
        out["placeholder"] = t.placeholder;
        return out;
    }

    Json Rename(const Json &params, std::string *error) {
        const std::string uri = UriOf(params);
        const std::vector<NixLspTextEdit> edits =
            NixLspRename(Analysis(params), LineOf(params), ColOf(params), params.get("newName").as_string(), error);
        if (!error->empty()) return Json();
        Json changes = Json::Object();
        changes[uri] = EditsJson(edits);
        Json out = Json::Object();
        out["changes"] = changes;
        return out;
    }

    Json SignatureHelp(const Json &params) {
        EnsureBuiltinDocs();
        const NixLspSignatureHelp sh = NixLspSignatureAt(Analysis(params), LineOf(params), ColOf(params));
        if (!sh.found) return Json();
        Json parameters = Json::Array();
        for (const std::string &param : sh.params) {
            Json p = Json::Object();
            p["label"] = param;
            parameters.push_back(p);
        }
        Json signature = Json::Object();
        signature["label"] = sh.label;
        if (!sh.documentation.empty()) {
            Json doc = Json::Object();
            doc["kind"] = "markdown";
            doc["value"] = sh.documentation;
            signature["documentation"] = doc;
        }
        signature["parameters"] = parameters;
        Json signatures = Json::Array();
        signatures.push_back(signature);
        Json out = Json::Object();
        out["signatures"] = signatures;
        out["activeSignature"] = 0;
        out["activeParameter"] = sh.active_param;
        return out;
    }
};

bool ReadInput(const std::string &path, std::string *text) {
    if (path == "-") {
        text->assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        return true;
    }
    bool ok = false;
    *text = ReadFile(path, &ok);
    if (!ok) std::fprintf(stderr, "mep-nix-lsp: cannot read %s\n", path.c_str());
    return ok;
}

int Lint(const std::vector<std::string> &paths) {
    bool failed = false;
    bool unreadable = false;
    for (const std::string &path : paths) {
        std::string text;
        if (!ReadInput(path, &text)) {
            unreadable = true;
            continue;
        }
        NixLspOptions opts;
        opts.origin = path;
        const auto doc = NixLspAnalyze(SplitLines(text), opts);
        for (const NixLspDiagnostic &d : NixLspDiagnostics(*doc)) {
            std::printf("%s\n", NixLspFormatDiagnostic(path, d).c_str());
            if (NixLspDiagnosticFails(d)) failed = true;
        }
    }
    if (unreadable) return 2;
    return failed ? 1 : 0;
}

std::string OneLine(const std::string &s) {
    std::string out;
    for (char c : s) out += c == '\n' ? ' ' : c;
    return out;
}

int NixCheck(const std::vector<std::string> &paths) {
    for (const std::string &path : paths) {
        std::string text;
        if (!ReadInput(path, &text)) {
            std::printf("unreadable\n");
            continue;
        }
        NixLspOptions opts;
        opts.origin = path;
        const NixLspCheck c = NixLspNixCheck(text, opts);
        if (c.parse_error) {
            std::printf("parse\t%d\t%s\n", c.offset, OneLine(c.message).c_str());
        } else if (!c.undefined.empty()) {
            std::printf("undefined");
            for (const auto &u : c.undefined) std::printf("\t%d\t%s", u.first, u.second.c_str());
            std::printf("\n");
        } else {
            std::printf("ok\n");
        }
    }
    std::fflush(stdout);
    return 0;
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
        if (first == "--nix-check") {
            if (rest.empty()) rest.emplace_back("-");
            return NixCheck(rest);
        }
        if (first == "--help" || first == "-h") {
            std::printf("usage: mep-nix-lsp                        run the language server on stdio\n"
                        "       mep-nix-lsp --lint FILE...         print diagnostics (FILE '-' reads stdin)\n"
                        "       mep-nix-lsp --nix-check FILE...    what nix-instantiate --parse would report\n");
            return 0;
        }
        if (first != "--stdio") {
            std::fprintf(stderr, "mep-nix-lsp: unknown argument %s (try --help)\n", first.c_str());
            return 2;
        }
    }
    NixLanguageServer server;
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

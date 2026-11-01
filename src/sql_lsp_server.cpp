// `mep-sql-lsp`: the wire half of mep's own SQL language server (SQLite's
// dialect), and a command-line linter. Speaks LSP over stdio; every answer
// comes from sql_lsp.h's pure analysis functions over sql_ast.h's parser.
//
// The schema it checks names against is the document's own DDL, the other
// .sql files of the workspace, and -- when one of the file's leading
// comment lines says `-- database: PATH` (relative to the file's
// directory, `~` expanded) -- that SQLite database's schema, read by
// running `sqlite3 -readonly -batch -json PATH 'SELECT ... FROM
// sqlite_schema'` and cached by the file's mtime. Nothing else is ever
// executed and the database is never written; without sqlite3 or the file
// the header gets an Information diagnostic and the checks fall back to
// what the document declares.
//
//   mep-sql-lsp                         the language server on stdio
//   mep-sql-lsp --lint FILE...          diagnostics as path:line:col: severity: message [code];
//                                       exit 1 on any error or warning ('-' reads stdin)
//   mep-sql-lsp --describe NAME [--database PATH]
//                                       plain-text docs for a function, PRAGMA, statement or keyword
//                                       (and with a database, a table/view or column); exit 1 and
//                                       print nothing when NAME is unknown
//   mep-sql-lsp --chunks FILE / --tokens FILE
//                                       the parser's statements and errors / its tokens, for
//                                       tools/sql_parse_check.py
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
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "json.h"
#include "rpc_framing.h"
#include "sql_lsp.h"

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

std::string JoinLines(const std::vector<std::string> &lines) {
    std::string t;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) t += '\n';
        t += lines[i];
    }
    return t;
}

// --- the `-- database:` schema ------------------------------------------------

struct DbCacheEntry {
    long long mtime = -1;
    long long size = -1;
    std::vector<SqlLspObject> objects;
    std::string problem;
};

std::map<std::string, DbCacheEntry> &DbCache() {
    static std::map<std::string, DbCacheEntry> cache;
    return cache;
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

/** @brief A database's schema objects (cached by mtime); `problem` says why there are none. */
const DbCacheEntry &LoadDatabase(const std::string &path) {
    DbCacheEntry &e = DbCache()[path];
#if defined(_WIN32)
    e.problem = "reading a database schema is not supported on this platform";
    return e;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        e = DbCacheEntry();
        e.problem = "database " + path + " does not exist";
        return e;
    }
    const long long mtime = static_cast<long long>(st.st_mtime);
    const long long size = static_cast<long long>(st.st_size);
    if (e.mtime == mtime && e.size == size) return e;
    e = DbCacheEntry();
    e.mtime = mtime;
    e.size = size;
    int status = -1;
    const std::string out = RunCapture({"sqlite3", "-readonly", "-batch", "-json", path,
                                        "SELECT type, name, tbl_name, sql FROM sqlite_schema"},
                                       &status);
    if (status == 127 || status < 0) {
        e.problem = "sqlite3 is not on PATH, so the schema of " + path + " cannot be read";
        e.mtime = -1;  // try again next time (sqlite3 may be installed meanwhile)
        return e;
    }
    if (status != 0) {
        e.problem = "sqlite3 could not read " + path + " as a database";
        return e;
    }
    std::vector<std::vector<std::string>> rows;
    Json j;
    size_t a = out.find_first_not_of(" \t\r\n");
    if (a != std::string::npos && Json::Parse(out, &j) && j.is_array()) {
        for (const Json &r : j.items()) {
            rows.push_back({r.get("type").as_string(), r.get("name").as_string(), r.get("tbl_name").as_string(),
                            r.get("sql").as_string()});
        }
    }
    e.objects = SqlLspSchemaObjects(rows, path);
    return e;
#endif
}

std::string ExpandPath(const std::string &raw, const std::string &base_dir) {
    std::string p = raw;
    if (p.size() >= 2 && ((p.front() == '"' && p.back() == '"') || (p.front() == '\'' && p.back() == '\''))) {
        p = p.substr(1, p.size() - 2);
    }
    if (!p.empty() && p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        const char *home = std::getenv("HOME");
        p = std::string(home ? home : "") + p.substr(1);
    }
    if (!p.empty() && p[0] != '/' && !base_dir.empty()) p = base_dir + "/" + p;
    return p;
}

std::string DirOf(const std::string &path) {
    const size_t s = path.find_last_of('/');
    if (s == std::string::npos) return ".";
    return s == 0 ? "/" : path.substr(0, s);
}

/** @brief Fills the database half of a context from the document's `-- database:` header. */
void AddDatabase(const std::vector<std::string> &lines, const std::string &doc_path, SqlLspContext *ctx) {
    const SqlLspDatabaseHeader h = SqlLspFindDatabaseHeader(lines);
    if (!h.found) return;
    const std::string path = ExpandPath(h.path, doc_path.empty() ? std::string() : DirOf(doc_path));
    ctx->database_path = path;
    if (path == ":memory:") return;
    const DbCacheEntry &e = LoadDatabase(path);
    if (!e.problem.empty()) {
        ctx->database_problem = e.problem;
        return;
    }
    ctx->database = e.objects;
    ctx->database_loaded = true;
}

// --- JSON shorthands --------------------------------------------------

Json Position(int line, int character) {
    Json p = Json::Object();
    p["line"] = line;
    p["character"] = character;
    return p;
}

Json RangeOf(const SqlLspRange &range) {
    Json r = Json::Object();
    r["start"] = Position(range.line, range.col);
    r["end"] = Position(range.end_line, range.end_col);
    return r;
}

// --- Server -----------------------------------------------------------

class SqlLanguageServer {
public:
    void HandleMessage(const std::string &body) {
        Json msg;
        if (!Json::Parse(body, &msg)) return;
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
            const std::string uri = UriOf(params);
            workspace_[uri] = SqlLspScriptObjects(JoinLines(Doc(uri)), uri);
            Publish(uri);
            return;
        }
        if (method == "textDocument/didOpen") {
            const std::string uri = UriOf(params);
            docs_[uri] = SplitLines(params.get("textDocument").get("text").as_string());
            Publish(uri);
            return;
        }
        if (method == "textDocument/didChange") {
            const std::string uri = UriOf(params);
            const Json &changes = params.get("contentChanges");
            if (changes.is_array() && changes.size() > 0) {
                docs_[uri] = SplitLines(changes.items().back().get("text").as_string());
            }
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
            // rangeFormatting formats the whole document: a statement's layout depends on all of it.
            std::string error;
            Json result = Formatting(params, &error);
            if (!error.empty()) {
                ReplyError(msg.get("id"), -32803, error);
            } else {
                Reply(msg.get("id"), result);
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
    std::map<std::string, std::vector<std::string>> docs_;
    std::map<std::string, std::vector<SqlLspObject>> workspace_;  // uri -> the objects that file declares
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
        sync["save"] = true;

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
        info["name"] = "mep-sql-lsp";
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

    static std::string UriFor(const std::string &path) { return "file://" + path; }

    /** @brief Indexes the workspace's .sql files (skipping build output and hidden directories). */
    void Index(const std::string &root) {
#if !defined(_WIN32)
        size_t budget = 20000;
        std::vector<std::string> stack = {root};
        while (!stack.empty() && budget > 0) {
            const std::string dir = stack.back();
            stack.pop_back();
            DIR *d = opendir(dir.c_str());
            if (!d) continue;
            while (dirent *e = readdir(d)) {
                const std::string name = e->d_name;
                if (name.empty() || name[0] == '.' || name == "node_modules" || name == "target" || name == "out" ||
                    name == "build")
                    continue;
                const std::string path = dir + "/" + name;
                struct stat st;
                if (stat(path.c_str(), &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    stack.push_back(path);
                    continue;
                }
                const size_t dot = name.find_last_of('.');
                const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
                if (ext != "sql" && ext != "SQL") continue;
                if (st.st_size > 2 * 1024 * 1024 || budget == 0) continue;
                budget--;
                bool ok = false;
                const std::string text = ReadFile(path, &ok);
                if (ok) workspace_[UriFor(path)] = SqlLspScriptObjects(text, UriFor(path));
            }
            closedir(d);
        }
#else
        (void)root;
#endif
    }

    const std::vector<std::string> &Doc(const std::string &uri) const {
        static const std::vector<std::string> kEmpty{std::string()};
        const auto it = docs_.find(uri);
        return it == docs_.end() ? kEmpty : it->second;
    }

    /** @brief The schema context of a document: the other workspace files and its `-- database:` file. */
    SqlLspContext ContextFor(const std::string &uri) const {
        SqlLspContext ctx;
        for (const auto &w : workspace_) {
            if (w.first == uri) continue;
            for (const SqlLspObject &o : w.second) {
                SqlLspObject copy = o;
                const std::string p = PathOf(w.first);
                const size_t s = p.find_last_of('/');
                copy.origin = s == std::string::npos ? p : p.substr(s + 1);
                ctx.workspace.push_back(copy);
            }
        }
        AddDatabase(Doc(uri), PathOf(uri), &ctx);
        return ctx;
    }

    static std::string UriOf(const Json &params) { return params.get("textDocument").get("uri").as_string(); }
    static int LineOf(const Json &params) { return params.get("position").get("line").as_int(); }
    static int ColOf(const Json &params) { return params.get("position").get("character").as_int(); }

    void Publish(const std::string &uri) {
        Json arr = Json::Array();
        for (const SqlLspDiagnostic &d : SqlLspDiagnostics(Doc(uri), ContextFor(uri))) {
            Json j = Json::Object();
            j["range"] = RangeOf(d.range);
            j["severity"] = static_cast<int>(d.severity);
            j["code"] = d.code;
            j["source"] = "sql";
            j["message"] = d.message;
            arr.push_back(j);
        }
        Json note = Json::Object();
        note["uri"] = uri;
        note["diagnostics"] = arr;
        Notify("textDocument/publishDiagnostics", note);
    }

    Json Completion(const Json &params) const {
        const int line = LineOf(params);
        const std::string uri = UriOf(params);
        const std::vector<SqlLspCompletionItem> items =
            SqlLspCompletions(Doc(uri), line, ColOf(params), ContextFor(uri));
        Json arr = Json::Array();
        for (size_t i = 0; i < items.size(); i++) {
            const SqlLspCompletionItem &it = items[i];
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

    Json HoverAt(const Json &params) const {
        const std::string uri = UriOf(params);
        const SqlLspHoverInfo info = SqlLspHover(Doc(uri), LineOf(params), ColOf(params), ContextFor(uri));
        if (!info.found) return Json();
        Json contents = Json::Object();
        contents["kind"] = "markdown";
        contents["value"] = info.text;
        Json out = Json::Object();
        out["contents"] = contents;
        out["range"] = RangeOf(info.range);
        return out;
    }

    Json Symbols(const Json &params) const {
        const std::vector<SqlLspSymbol> syms = SqlLspSymbols(Doc(UriOf(params)));
        std::vector<Json> nodes(syms.size());
        for (size_t i = syms.size(); i-- > 0;) {
            const SqlLspSymbol &s = syms[i];
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

    Json Folds(const Json &params) const {
        Json arr = Json::Array();
        for (const SqlLspFold &f : SqlLspFolds(Doc(UriOf(params)))) {
            Json j = Json::Object();
            j["startLine"] = f.start_line;
            j["endLine"] = f.end_line;
            if (!f.kind.empty()) j["kind"] = f.kind;
            arr.push_back(j);
        }
        return arr;
    }

    Json Definition(const Json &params) const {
        const std::string uri = UriOf(params);
        Json arr = Json::Array();
        for (const SqlLspLocation &loc : SqlLspDefinition(Doc(uri), LineOf(params), ColOf(params), ContextFor(uri))) {
            Json j = Json::Object();
            j["uri"] = loc.uri.empty() ? uri : loc.uri;
            j["range"] = RangeOf(loc.range);
            arr.push_back(j);
        }
        if (arr.size() == 0) return Json();
        return arr;
    }

    Json References(const Json &params) const {
        const std::string uri = UriOf(params);
        const bool include_declaration =
            !params.contains("context") || params.get("context").get("includeDeclaration").as_bool();
        Json arr = Json::Array();
        for (const SqlLspReference &ref : SqlLspReferences(Doc(uri), LineOf(params), ColOf(params), ContextFor(uri))) {
            if (ref.is_definition && !include_declaration) continue;
            Json j = Json::Object();
            j["uri"] = uri;
            j["range"] = RangeOf(ref.range);
            arr.push_back(j);
        }
        return arr;
    }

    Json Highlights(const Json &params) const {
        const std::string uri = UriOf(params);
        Json arr = Json::Array();
        for (const SqlLspReference &ref : SqlLspReferences(Doc(uri), LineOf(params), ColOf(params), ContextFor(uri))) {
            Json j = Json::Object();
            j["range"] = RangeOf(ref.range);
            j["kind"] = ref.is_write ? 3 : 2;
            arr.push_back(j);
        }
        return arr;
    }

    Json PrepareRename(const Json &params) const {
        const std::string uri = UriOf(params);
        const SqlLspRenameTarget t = SqlLspPrepareRename(Doc(uri), LineOf(params), ColOf(params), ContextFor(uri));
        if (!t.found) return Json();
        Json out = Json::Object();
        out["range"] = RangeOf(t.range);
        out["placeholder"] = t.placeholder;
        return out;
    }

    Json Rename(const Json &params, std::string *error) const {
        const std::string uri = UriOf(params);
        const std::vector<SqlLspTextEdit> edits = SqlLspRename(Doc(uri), LineOf(params), ColOf(params),
                                                               params.get("newName").as_string(), error,
                                                               ContextFor(uri));
        if (!error->empty()) return Json();
        Json arr = Json::Array();
        for (const SqlLspTextEdit &e : edits) {
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

    Json Formatting(const Json &params, std::string *error) const {
        const Json &opts = params.get("options");
        const bool spaces = opts.get("insertSpaces").as_bool(true);
        const int n = opts.get("tabSize").as_int(4);
        Json arr = Json::Array();
        for (const SqlLspTextEdit &e : SqlLspFormat(Doc(UriOf(params)), n > 0 && n <= 16 ? n : 4, !spaces, error)) {
            Json j = Json::Object();
            j["range"] = RangeOf(e.range);
            j["newText"] = e.new_text;
            arr.push_back(j);
        }
        return arr;
    }

    Json SignatureHelp(const Json &params) const {
        const SqlLspSignatureHelp sh = SqlLspSignatureAt(Doc(UriOf(params)), LineOf(params), ColOf(params));
        if (!sh.found) return Json();
        Json signatures = Json::Array();
        for (const SqlLspSignature &s : sh.signatures) {
            Json parameters = Json::Array();
            for (const std::string &param : s.params) {
                Json p = Json::Object();
                p["label"] = param;
                parameters.push_back(p);
            }
            Json signature = Json::Object();
            signature["label"] = s.label;
            if (!sh.documentation.empty()) signature["documentation"] = sh.documentation;
            signature["parameters"] = parameters;
            signatures.push_back(signature);
        }
        Json out = Json::Object();
        out["signatures"] = signatures;
        out["activeSignature"] = sh.active_signature;
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
                std::fprintf(stderr, "mep-sql-lsp: cannot read %s\n", path.c_str());
                unreadable = true;
                continue;
            }
        }
        const std::vector<std::string> lines = SplitLines(text);
        SqlLspContext ctx;
        AddDatabase(lines, path == "-" ? std::string() : path, &ctx);
        for (const SqlLspDiagnostic &d : SqlLspDiagnostics(lines, ctx)) {
            std::printf("%s\n", SqlLspFormatDiagnostic(path, d).c_str());
            if (SqlLspDiagnosticFails(d)) failed = true;
        }
    }
    if (unreadable) return 2;
    return failed ? 1 : 0;
}

/** @brief `--chunks FILE...`: one line per statement: start, end, ok|syntax|action, error offset, message. */
int Chunks(const std::vector<std::string> &paths) {
    for (const std::string &path : paths) {
        bool ok = false;
        const std::string text = ReadFile(path, &ok);
        if (!ok) return 2;
        const SqlParse p = SqlParseScript(text);
        for (size_t ci = 0; ci < p.chunks.size(); ci++) {
            const SqlChunk &c = p.chunks[ci];
            const SqlSyntaxError *err = nullptr;
            for (const SqlSyntaxError &e : p.errors) {
                if (e.chunk == static_cast<int>(ci)) {
                    err = &e;
                    break;
                }
            }
            if (!err) {
                std::printf("%d\t%d\tok\t-1\t\n", c.start, c.end);
                continue;
            }
            std::string msg;
            for (char ch : err->message) {
                if (ch == '\n') {
                    msg += "\\n";
                } else {
                    msg += ch;
                }
            }
            std::printf("%d\t%d\t%s\t%d\t%s\n", c.start, c.end, err->syntax ? "syntax" : "action",
                        err->offset < 0 ? -1 : err->offset - c.start, msg.c_str());
        }
    }
    return 0;
}

/** @brief `--tokens FILE`: every token but whitespace: start, end, type (comment / eof / other). */
int Tokens(const std::string &path) {
    bool ok = false;
    const std::string text = ReadFile(path, &ok);
    if (!ok) return 2;
    for (const SqlToken &t : SqlTokenize(text)) {
        if (t.type == SqlTok::Space) continue;
        const char *kind = t.type == SqlTok::Comment ? "comment" : t.type == SqlTok::Eof ? "eof" : "token";
        std::printf("%d\t%d\t%s\n", t.start, t.start + t.len, kind);
    }
    return 0;
}

int Describe(const std::string &name, const std::string &database) {
    std::vector<SqlLspObject> objects;
    if (!database.empty()) {
        const DbCacheEntry &e = LoadDatabase(ExpandPath(database, std::string()));
        objects = e.objects;
    }
    const std::string text = SqlLspDescribe(name, objects);
    if (text.empty()) return 1;
    std::fwrite(text.data(), 1, text.size(), stdout);
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
        if (first == "--describe") {
            if (rest.empty()) return 1;
            std::string db;
            for (size_t i = 1; i + 1 < rest.size(); i++) {
                if (rest[i] == "--database") db = rest[i + 1];
            }
            return Describe(rest[0], db);
        }
        if (first == "--chunks") return Chunks(rest);
        if (first == "--tokens") return rest.empty() ? 2 : Tokens(rest[0]);
        if (first == "--help" || first == "-h") {
            std::printf("usage: mep-sql-lsp                        run the language server on stdio\n"
                        "       mep-sql-lsp --lint FILE...         print diagnostics (FILE '-' reads stdin)\n"
                        "       mep-sql-lsp --describe NAME [--database PATH]\n"
                        "                                          describe a function, PRAGMA, statement or keyword\n"
                        "                                          (or, with a database, a table or column)\n");
            return 0;
        }
        if (first != "--stdio") {
            std::fprintf(stderr, "mep-sql-lsp: unknown argument %s (try --help)\n", first.c_str());
            return 2;
        }
    }
    SqlLanguageServer server;
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

// `mep-python-lsp`: the wire half of mep's own Python language server.
// Speaks LSP over stdio; every answer comes from python_lsp.h's pure
// analysis functions, which is where the actual Python knowledge lives
// (python_ast.h is the tokenizer and parser under those).
//
// Standalone on purpose, rather than a set of Lua callbacks inside mep,
// for the same three reasons org_lsp_server.cpp gives: it is the shape
// every other server in mep.lsp_servers already has, so kBuiltinLsp
// attaches to it through the same code path as clangd with no special
// case anywhere; a crash or a pathological document costs a subprocess
// rather than the editor; and it works in any LSP client, so a Python
// file edited outside mep gets the same diagnostics.
//
// Deliberately single-threaded and synchronous. Every request is a parse
// plus a walk of one already-in-memory document -- a few milliseconds for
// files far larger than anyone edits by hand (measured at ~6 ms for a
// 14,000-line file), so there is no work queue, no cancellation handling
// and no background indexing to get wrong.
//
// Transport notes:
//   - Content-Length framing via rpc_framing.h (PumpRpcFrames /
//     FrameRpcMessage), the same helper agent_rpc.cpp uses.
//   - Text synchronization is full (TextDocumentSyncKind.Full = 1):
//     re-analysis is a single linear pass, so incremental updates would
//     buy nothing but a harder-to-get-right document store.
//   - positionEncoding is "utf-8", i.e. positions are byte offsets, to
//     match what mep's client reads `character` as. Python source is
//     full of non-ASCII string literals and identifiers may be
//     non-ASCII too, so declaring UTF-16 and not honoring it would put
//     every diagnostic on such a line in the wrong column.

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "json.h"
#include "python_lsp.h"
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

// --- Interpreter introspection ----------------------------------------
//
// The baked-in vocabulary covers the standard library and nothing else,
// so hover on `np.array` or `plt.plot` would otherwise say nothing. When
// the name under the cursor traces back to an import
// (PythonLspQualifiedName), and that import is not one the table already
// covers, the server asks a real interpreter: import the module, walk to
// the object, print its signature and docstring. That does run the
// imported module's top-level code -- the same code running the file
// would -- and only on an explicit hover request, never while typing.
// It is a fallback, not a dependency: with no interpreter found, or the
// module not installed, hover answers exactly as it did before.
//
// Which interpreter: `pythonPath` from initializationOptions or the
// `python` settings section, else $MEP_PYTHON, else the active
// $VIRTUAL_ENV, else the nearest `.venv`/`venv` above the document, else
// `python3`/`python` from PATH -- so a project venv's numpy is the one
// documented, not whatever the system happens to have.

// Prints "<header>\n\n<docstring>" for the dotted path in argv[1], or
// exits non-zero when it cannot be imported. stdout is swapped for
// stderr while importing so a chatty module cannot corrupt the answer.
const char *const kIntrospectScript = R"PY(
import sys, os, importlib, inspect
os.environ.setdefault('MPLBACKEND', 'Agg')
out = sys.stdout
sys.stdout = sys.stderr
q = sys.argv[1]
parts = q.split('.')
try:
    obj = importlib.import_module(parts[0])
    for i in range(1, len(parts)):
        try:
            obj = getattr(obj, parts[i])
        except AttributeError:
            obj = importlib.import_module('.'.join(parts[:i + 1]))
except BaseException:
    sys.exit(1)
try:
    sig = str(inspect.signature(obj))
except BaseException:
    sig = ''
if inspect.ismodule(obj):
    head = 'module ' + obj.__name__
elif inspect.isclass(obj):
    head = 'class ' + q + sig
elif callable(obj):
    head = 'def ' + q + sig if sig else q
else:
    r = repr(obj)
    head = q + ': ' + type(obj).__name__ + (' = ' + r if len(r) <= 80 else '')
own = inspect.ismodule(obj) or inspect.isclass(obj) or callable(obj) or \
    getattr(obj, '__doc__', None) != getattr(type(obj), '__doc__', None)
doc = (inspect.getdoc(obj) or '') if own else ''
lines = doc.splitlines()
if len(lines) > 200:
    lines = lines[:200] + ['...']
text = head + ('\n\n' + '\n'.join(lines) if lines else '')
src = getattr(inspect.getmodule(obj), '__name__', '')
if src and not inspect.ismodule(obj) and src != q.rsplit('.', 1)[0]:
    text += '\n\nDefined in ' + src
out.buffer.write(text.encode('utf-8', 'replace'))
)PY";

// Lists the attributes of the object at the dotted path in argv[1], one
// per line as "name<TAB>kind<TAB>detail<TAB>doc" -- kind a
// CompletionItemKind number, detail a signature, doc the docstring's first
// line -- or exits non-zero when it cannot be imported. Same stdout swap
// as above. `inspect.signature` on every member of numpy costs well under
// a second, and the answer is cached for the session.
const char *const kMembersScript = R"PY(
import sys, os, importlib, inspect, warnings
warnings.simplefilter('ignore')
os.environ.setdefault('MPLBACKEND', 'Agg')
out = sys.stdout
sys.stdout = sys.stderr
parts = sys.argv[1].split('.')
try:
    obj = importlib.import_module(parts[0])
    for i in range(1, len(parts)):
        try:
            obj = getattr(obj, parts[i])
        except AttributeError:
            obj = importlib.import_module('.'.join(parts[:i + 1]))
except BaseException:
    sys.exit(1)
def clean(s):
    return ' '.join(str(s).split())
rows = []
for name in sorted(dir(obj), key=lambda n: (n.startswith('_'), n.lower())):
    try:
        val = getattr(obj, name)
    except BaseException:
        continue
    if inspect.ismodule(val):
        kind, detail = 9, 'module'
    elif inspect.isclass(val):
        kind, detail = 7, 'class'
    elif callable(val):
        kind = 2 if inspect.isclass(obj) else 3
        try:
            detail = str(inspect.signature(val))
        except BaseException:
            detail = '(...)'
    else:
        kind, detail = 21 if name.isupper() else 6, type(val).__name__
    doc = ''
    if kind != 6 and kind != 21:
        try:
            doc = (inspect.getdoc(val) or '').strip().split('\n\n')[0]
        except BaseException:
            pass
    detail, doc = clean(detail)[:160], clean(doc)[:200]
    rows.append(name + '\t' + str(kind) + '\t' + detail + '\t' + doc)
out.buffer.write('\n'.join(rows).encode('utf-8', 'replace'))
)PY";

// A cold `import matplotlib.pyplot` can take a few seconds (font cache);
// anything slower is treated as a hang and killed.
constexpr int kIntrospectTimeoutMs = 10000;

/** @brief Reports whether a path names an existing regular file. */
bool IsFile(const std::filesystem::path &p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) && !ec;
}

/** @brief A virtual environment's interpreter, or "" when the directory is not one. */
std::string VenvPython(const std::filesystem::path &venv) {
#if defined(_WIN32)
    const std::filesystem::path py = venv / "Scripts" / "python.exe";
#else
    const std::filesystem::path py = venv / "bin" / "python";
#endif
    return IsFile(py) ? py.string() : std::string();
}

/** @brief Picks the interpreter to introspect with (see the section comment for the order). */
std::string FindPython(const std::string &configured, const std::string &doc_dir) {
    if (!configured.empty()) return configured;
    if (const char *env = std::getenv("MEP_PYTHON"); env != nullptr && env[0] != '\0') return env;
    if (const char *venv = std::getenv("VIRTUAL_ENV"); venv != nullptr && venv[0] != '\0') {
        const std::string py = VenvPython(venv);
        if (!py.empty()) return py;
    }
    if (!doc_dir.empty()) {
        std::filesystem::path dir(doc_dir);
        for (int depth = 0; depth < 32 && !dir.empty(); depth++) {
            for (const char *name : {".venv", "venv"}) {
                const std::string py = VenvPython(dir / name);
                if (!py.empty()) return py;
            }
            const std::filesystem::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    }
#if defined(_WIN32)
    return "python";
#else
    return "python3";
#endif
}

#if defined(_WIN32)
/** @brief Base64-encodes bytes, so the script survives cmd.exe quoting intact. */
std::string Base64(const std::string &in) {
    static const char *const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < in.size() ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}
#endif

/**
 * @brief Runs one of the introspection scripts for one dotted path.
 * @param python the interpreter to run
 * @param script kIntrospectScript or kMembersScript
 * @param cwd the document's directory, so sibling modules import ("" leaves the cwd alone)
 * @param qualified a dotted identifier path (PythonLspQualifiedName output, so never shell-special)
 * @return the hover text, or "" on any failure (not installed, import error, timeout)
 */
std::string Introspect(const std::string &python, const char *script, const std::string &cwd,
                       const std::string &qualified) {
#if defined(_WIN32)
    // No timeout on this path: _popen offers none. The script is passed
    // base64-encoded so no quoting rule can mangle it.
    std::string cmd = "cd /d \"" + cwd + "\" && \"" + python +
                      "\" -c \"import base64;exec(base64.b64decode('" + Base64(script) + "'))\" " +
                      qualified + " 2>NUL";
    if (cwd.empty()) cmd = cmd.substr(cmd.find("&& ") + 3);
    FILE *pipe = _popen(cmd.c_str(), "rb");
    if (pipe == nullptr) return std::string();
    std::string out;
    char buf[4096];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, got);
    return _pclose(pipe) == 0 ? out : std::string();
#else
    int fds[2];
    if (pipe(fds) != 0) return std::string();
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return std::string();
    }
    if (pid == 0) {
        // Own process group, so a timeout can kill anything the import
        // itself spawned along with the interpreter.
        setpgid(0, 0);
        const int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        execlp(python.c_str(), python.c_str(), "-c", script, qualified.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    bool timed_out = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kIntrospectTimeoutMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                                std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            timed_out = true;
            break;
        }
        pollfd pfd{fds[0], POLLIN, 0};
        const int ready = poll(&pfd, 1, static_cast<int>(left.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            timed_out = ready == 0;
            break;
        }
        char buf[4096];
        const ssize_t got = read(fds[0], buf, sizeof(buf));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        out.append(buf, static_cast<size_t>(got));
    }
    close(fds[0]);
    if (timed_out) kill(-pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::string();
    return out;
#endif
}

/** @brief Parses kMembersScript's output into completion members. */
std::vector<PythonLspExternalMember> ParseMembers(const std::string &text) {
    std::vector<PythonLspExternalMember> members;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string row = text.substr(start, end - start);
        start = end + 1;
        std::string fields[4];
        size_t at = 0;
        for (int f = 0; f < 4; f++) {
            const size_t tab = f < 3 ? row.find('\t', at) : std::string::npos;
            fields[f] = row.substr(at, tab == std::string::npos ? std::string::npos : tab - at);
            if (tab == std::string::npos) break;
            at = tab + 1;
        }
        if (fields[0].empty()) continue;
        PythonLspExternalMember m;
        m.name = fields[0];
        m.kind = std::atoi(fields[1].c_str());
        if (m.kind <= 0) m.kind = static_cast<int>(PythonLspKind::Field);
        m.detail = fields[2];
        m.doc = fields[3];
        members.push_back(std::move(m));
    }
    return members;
}

/** @brief The top-level package of a dotted path ("numpy" for "numpy.linalg.norm"). */
std::string RootModule(const std::string &qualified) { return qualified.substr(0, qualified.find('.')); }

// --- Server -----------------------------------------------------------

class PythonLanguageServer {
public:
    /** @brief Feeds one complete JSON-RPC message body to the server. */
    void HandleMessage(const std::string &body) {
        Json msg;
        if (!Json::Parse(body, &msg)) return;  // a peer that cannot frame JSON is not one we can answer
        const std::string method = msg.get("method").as_string();
        const Json &params = msg.get("params");
        const bool is_request = msg.contains("id");

        if (method == "initialize") {
            // The one setting this server takes: a column limit for the
            // line-too-long hint, off unless the client asks for it.
            const Json &init = params.get("initializationOptions");
            if (init.is_object() && init.contains("maxLineLength")) {
                max_line_length_ = init.get("maxLineLength").as_int();
            }
            if (init.is_object() && init.contains("pythonPath")) {
                python_path_ = init.get("pythonPath").as_string();
            }
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
            const Json &settings = params.get("settings").get("python");
            if (settings.is_object() && settings.contains("maxLineLength")) {
                max_line_length_ = settings.get("maxLineLength").as_int();
                for (const auto &doc : docs_) Publish(doc.first);
            }
            if (settings.is_object() && settings.contains("pythonPath")) {
                python_path_ = settings.get("pythonPath").as_string();
                introspect_cache_.clear();
                members_cache_.clear();
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
        } else if (method == "textDocument/semanticTokens/full") {
            Reply(msg.get("id"), SemanticTokens(params));
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
    std::string python_path_;  // initializationOptions / settings `pythonPath`, "" to search
    mutable std::map<std::string, std::string> introspect_cache_;  // see IntrospectHover
    // see MembersOf; an empty vector records a failure, so it is not retried
    mutable std::map<std::string, std::vector<PythonLspExternalMember>> members_cache_;
    bool running_ = true;
    bool shutting_down_ = false;
    int exit_code_ = 1;
    int max_line_length_ = 0;

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
        // `.` is the only character that opens a context a typed prefix
        // would not: everything else this server offers also matches a
        // plain word prefix.
        Json triggers = Json::Array();
        triggers.push_back(".");
        completion["triggerCharacters"] = triggers;

        Json signature = Json::Object();
        Json sig_triggers = Json::Array();
        sig_triggers.push_back("(");
        sig_triggers.push_back(",");
        signature["triggerCharacters"] = sig_triggers;

        Json rename = Json::Object();
        rename["prepareProvider"] = true;

        // Semantic tokens: the legend is this server's own token-type and
        // modifier order (PythonLspTokenType / PythonLspTokenModifier in
        // python_lsp.h -- the numbers on the wire are indexes into these
        // two arrays, so their order is the protocol here and must match
        // those enums exactly). Full documents only: re-analysis is one
        // linear pass over an in-memory document, so a delta protocol
        // would add a result cache to get wrong and save nothing.
        Json semantic = Json::Object();
        Json legend = Json::Object();
        Json token_types = Json::Array();
        for (const char *name : {"namespace", "type", "class", "parameter", "variable", "function", "method"}) {
            token_types.push_back(std::string(name));
        }
        Json token_modifiers = Json::Array();
        for (const char *name : {"declaration", "implicit"}) token_modifiers.push_back(std::string(name));
        legend["tokenTypes"] = token_types;
        legend["tokenModifiers"] = token_modifiers;
        semantic["legend"] = legend;
        semantic["full"] = true;
        semantic["range"] = false;

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
        caps["semanticTokensProvider"] = semantic;

        Json info = Json::Object();
        info["name"] = "mep-python-lsp";
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
    PythonLspOptions OptionsFor(const std::string &uri) const {
        PythonLspOptions opts;
        const std::string path = UriToPath(uri);
        if (!path.empty() && path[0] == '/') {
            const std::filesystem::path fs_path(path);
            opts.doc_dir = fs_path.parent_path().string();
            opts.file_name = fs_path.filename().string();
        }
        // An unsaved/untitled document has no directory to resolve
        // against, and guessing the server's own cwd would resolve
        // imports to files that have nothing to do with it.
        opts.check_files = !opts.doc_dir.empty();
        opts.max_line_length = max_line_length_;
        return opts;
    }

    /** @brief Analyzes a document and pushes the result to the client. */
    void Publish(const std::string &uri) {
        const std::vector<PythonLspDiagnostic> diags = PythonLspDiagnostics(Doc(uri), OptionsFor(uri));
        Json arr = Json::Array();
        for (const PythonLspDiagnostic &d : diags) {
            Json j = Json::Object();
            j["range"] = RangeOnLine(d.line, d.col_start, d.col_end);
            j["severity"] = static_cast<int>(d.severity);
            j["code"] = d.code;
            j["source"] = "python";
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

    /**
     * @brief The members of a third-party object (`numpy`, `matplotlib.pyplot`), as an interpreter lists them.
     *
     * The completion counterpart of IntrospectHover: one subprocess per
     * dotted path per session, failures cached too, so `np.` costs an
     * import once and is instant from then on.
     */
    const std::vector<PythonLspExternalMember> *MembersOf(const std::string &doc_dir,
                                                          const std::string &qualified) const {
        const std::string python = FindPython(python_path_, doc_dir);
        const std::string key = python + '\n' + doc_dir + '\n' + qualified;
        auto it = members_cache_.find(key);
        if (it == members_cache_.end()) {
            it = members_cache_.emplace(key, ParseMembers(Introspect(python, kMembersScript, doc_dir, qualified)))
                     .first;
        }
        return it->second.empty() ? nullptr : &it->second;
    }

    /** @brief Answers `textDocument/completion`. */
    Json Completion(const Json &params) const {
        const Request req = ReadRequest(params);
        PythonLspOptions opts = OptionsFor(req.uri);
        const std::string doc_dir = opts.doc_dir;
        opts.external_members = [this, doc_dir](const std::string &qualified) { return MembersOf(doc_dir, qualified); };
        const std::vector<PythonLspCompletionItem> items = PythonLspCompletions(Doc(req.uri), req.line, req.col, opts);
        Json arr = Json::Array();
        for (size_t i = 0; i < items.size(); i++) {
            const PythonLspCompletionItem &it = items[i];
            Json j = Json::Object();
            j["label"] = it.label;
            j["kind"] = static_cast<int>(it.kind);
            j["insertText"] = it.insert_text;
            j["insertTextFormat"] = 1;  // PlainText: never a snippet, see python_lsp.h
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

    /**
     * @brief Replaces a hover the built-in tables cannot answer with what an interpreter says about the name.
     *
     * Only for a name that traces back to an import of a module the
     * stdlib table does not cover (os.path keeps its instant, offline
     * answer); results, failures included, are cached per interpreter
     * and path, so each name costs at most one subprocess per session.
     */
    void IntrospectHover(const Request &req, PythonLspHoverInfo *info) const {
        const std::vector<std::string> &lines = Doc(req.uri);
        const std::string qualified = PythonLspQualifiedName(lines, req.line, req.col);
        if (qualified.empty() || PythonLspModuleMembers(RootModule(qualified)) != nullptr) return;
        const PythonLspOptions opts = OptionsFor(req.uri);
        const std::string python = FindPython(python_path_, opts.doc_dir);
        const std::string key = python + '\n' + opts.doc_dir + '\n' + qualified;
        auto it = introspect_cache_.find(key);
        if (it == introspect_cache_.end()) {
            it = introspect_cache_.emplace(key, Introspect(python, kIntrospectScript, opts.doc_dir, qualified)).first;
        }
        if (it->second.empty()) return;
        // A failed built-in hover still carries the range of the name it
        // looked at (it only fails after finding one), so that stays.
        info->found = true;
        info->text = it->second;
    }

    /** @brief Answers `textDocument/hover`. */
    Json HoverAt(const Json &params) const {
        const Request req = ReadRequest(params);
        PythonLspHoverInfo info = PythonLspHover(Doc(req.uri), req.line, req.col);
        IntrospectHover(req, &info);
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
    static Json SymbolNode(const std::vector<PythonLspSymbol> &syms, const std::vector<std::vector<int>> &children,
                           const std::vector<std::string> &lines, size_t index) {
        const PythonLspSymbol &s = syms[index];
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
        const std::vector<PythonLspSymbol> syms = PythonLspSymbols(lines);
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
        for (const PythonLspFold &f : PythonLspFolds(Doc(uri))) {
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
        const PythonLspLocation loc = PythonLspDefinition(Doc(req.uri), req.line, req.col, OptionsFor(req.uri));
        if (!loc.found) return Json();
        Json j = Json::Object();
        j["uri"] = loc.path.empty() ? req.uri : PathToUri(loc.path);
        j["range"] = RangeOnLine(loc.line, loc.col, loc.col);
        return j;
    }

    /** @brief Answers `textDocument/references`. */
    Json References(const Json &params) const {
        const Request req = ReadRequest(params);
        const PythonLspReferenceSet refs = PythonLspReferences(Doc(req.uri), req.line, req.col);
        const bool include_decl = !params.get("context").is_object() ||
                                  params.get("context").get("includeDeclaration").as_bool();
        Json arr = Json::Array();
        for (const PythonLspReference &r : refs.refs) {
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
        const PythonLspReferenceSet refs = PythonLspReferences(Doc(req.uri), req.line, req.col);
        Json arr = Json::Array();
        for (const PythonLspReference &r : refs.refs) {
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
        const PythonLspReferenceSet refs = PythonLspReferences(Doc(req.uri), req.line, req.col);
        if (!refs.found || !refs.rename_blocked_reason.empty() || refs.refs.empty()) return Json();
        return RangeOnLine(refs.refs.front().line, refs.refs.front().col_start, refs.refs.front().col_end);
    }

    /** @brief Answers `textDocument/rename` with a single-file WorkspaceEdit. */
    void Rename(const Json &id, const Json &params) const {
        const Request req = ReadRequest(params);
        const std::string new_name = params.get("newName").as_string();
        const PythonLspReferenceSet refs = PythonLspReferences(Doc(req.uri), req.line, req.col);
        if (!refs.found || refs.refs.empty()) {
            ReplyError(id, -32803, "Nothing to rename here");
            return;
        }
        if (!refs.rename_blocked_reason.empty()) {
            ReplyError(id, -32803, "Cannot rename: " + refs.rename_blocked_reason);
            return;
        }
        Json edits = Json::Array();
        for (const PythonLspReference &r : refs.refs) {
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
        const PythonLspSignature sig = PythonLspSignatureHelp(Doc(req.uri), req.line, req.col);
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

    /** @brief Answers `textDocument/semanticTokens/full`. */
    Json SemanticTokens(const Json &params) const {
        const std::string uri = params.get("textDocument").get("uri").as_string();
        const std::vector<PythonLspSemanticToken> tokens = PythonLspSemanticTokens(Doc(uri));
        // The wire format (LSP SemanticTokens.data): five integers per
        // token, each position stated as a delta from the previous token's
        // -- line delta, then a column delta *within the same line* or an
        // absolute column on a new one. Every token here is one
        // identifier, so none of them spans a line break and `length` is
        // simply its byte width.
        Json data = Json::Array();
        int prev_line = 0;
        int prev_col = 0;
        for (const PythonLspSemanticToken &t : tokens) {
            const int delta_line = t.line - prev_line;
            data.push_back(delta_line);
            data.push_back(delta_line == 0 ? t.col_start - prev_col : t.col_start);
            data.push_back(t.col_end - t.col_start);
            data.push_back(static_cast<int>(t.type));
            data.push_back(static_cast<int>(t.modifiers));
            prev_line = t.line;
            prev_col = t.col_start;
        }
        Json out = Json::Object();
        out["data"] = data;
        return out;
    }
};

}  // namespace

/**
 * @brief Runs the Python language server's stdio read loop until the client sends `exit` (or closes the stream).
 * @return the exit status LSP asks for: 0 after a `shutdown`/`exit` handshake, 1 otherwise
 */
int main() {
#if defined(_WIN32)
    // Without this the CRT translates '\n' to "\r\n" on the way out,
    // which corrupts every Content-Length byte count.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    PythonLanguageServer server;
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

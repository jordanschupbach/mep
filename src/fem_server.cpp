// `mep-fem`: the wire half of mep's finite-element solver
// (plans/CAD_FEM_PLAN.md Part 0.5). Speaks JSON-RPC 2.0 over stdio with
// Content-Length framing, exactly like the language servers this repo
// already ships; every answer comes from fem_solve.h's pure functions,
// which is where the actual mechanics lives.
//
// A separate process rather than a thread inside the editor, for reasons
// that are specific rather than stylistic:
//
//   - mep is deliberately single-threaded. src/job.h's own comment is
//     explicit that callbacks never fire off the main thread, so that
//     editor state can be touched from them without synchronization. A
//     solver thread would be the first thing in the codebase to violate
//     that, and it would do so while holding the largest data structures
//     in the process.
//   - A solve is the one operation here that can legitimately run for
//     minutes and allocate gigabytes. In-process, a model that turns out
//     to be too large takes the editor's unsaved buffers down with it;
//     out of process it costs a subprocess and an error message.
//   - JobManager already runs exactly this shape of child -- fork/exec,
//     Content-Length-framed raw stdout, completion on the main thread --
//     for every language server, so the editor side is a client of
//     machinery that is already load-bearing rather than new code.
//   - It gives a headless CLI solver for free (--solve below), which is
//     what the verification work in Part H needs: NAFEMS benchmarks and
//     convergence studies run in CI, with no display and no editor.
//
// Methods:
//   fem/solve     params: the model object (fem_model.h's JSON encoding)
//                 plus optional "solver" ("direct" | "iterative") and
//                 "ordering" ("natural" | "rcm" | "minimum-degree").
//                 Streams fem/progress notifications while it runs, then
//                 replies with fem_solve.h's result encoding.
//   fem/validate  params: a model. Replies with {ok, error} and solves
//                 nothing -- so the editor can tell someone their model
//                 is unrestrained without waiting for a factorization.
//   shutdown      replies null; exit then ends the process.

#include "fem_model.h"
#include "fem_solve.h"
#include "json.h"
#include "rpc_framing.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

void WriteMessage(const std::string &body) {
    const std::string framed = FrameRpcMessage(body);
    std::fwrite(framed.data(), 1, framed.size(), stdout);
    std::fflush(stdout);
}

void WriteResult(const Json &id, const std::string &result_json) {
    // The result is already-serialized JSON (fem_solve.h produces it that
    // way, and for a large mesh it is the bulk of the message), so it is
    // spliced in as text rather than parsed into a Json only to be dumped
    // straight back out.
    std::string body = "{\"jsonrpc\":\"2.0\",\"id\":";
    body += id.dump();
    body += ",\"result\":";
    body += result_json;
    body += "}";
    WriteMessage(body);
}

void WriteError(const Json &id, int code, const std::string &message) {
    Json error;
    error["code"] = Json(static_cast<double>(code));
    error["message"] = Json(message);
    Json root;
    root["jsonrpc"] = Json(std::string("2.0"));
    root["id"] = id;
    root["error"] = error;
    WriteMessage(root.dump());
}

void WriteProgressNotification(double fraction, const std::string &stage) {
    Json params;
    params["fraction"] = Json(fraction);
    params["stage"] = Json(stage);
    Json root;
    root["jsonrpc"] = Json(std::string("2.0"));
    root["method"] = Json(std::string("fem/progress"));
    root["params"] = params;
    WriteMessage(root.dump());
}

fem::SolverKind ParseSolverKind(const std::string &name) {
    if (name == "iterative" || name == "cg") return fem::SolverKind::Iterative;
    return fem::SolverKind::Direct;
}

num::Ordering ParseOrdering(const std::string &name) {
    if (name == "natural") return num::Ordering::Natural;
    if (name == "rcm" || name == "reverse-cuthill-mckee") return num::Ordering::ReverseCuthillMcKee;
    return num::Ordering::MinimumDegree;
}

// Shared by the JSON-RPC handler and the --solve CLI path, so the two can
// never drift into answering the same request differently.
std::string RunSolve(const Json &params, bool stream_progress) {
    fem::Model model;
    std::string error;
    if (!fem::ModelFromJson(params.dump(), &model, &error)) {
        fem::Result failed;
        failed.error = error;
        return fem::ResultToJson(failed);
    }
    fem::SolveOptions options;
    options.solver = ParseSolverKind(params.get("solver").as_string(""));
    options.ordering = ParseOrdering(params.get("ordering").as_string(""));
    if (params.contains("tolerance")) options.iterative_tolerance = params.get("tolerance").as_double(1e-10);
    if (stream_progress) options.progress = WriteProgressNotification;
    return fem::ResultToJson(fem::Solve(model, options));
}

void HandleMessage(const std::string &body, bool *should_exit) {
    Json request;
    if (!Json::Parse(body, &request)) {
        WriteError(Json(), -32700, "parse error: request is not valid JSON");
        return;
    }
    const Json &id = request.get("id");
    const std::string method = request.get("method").as_string("");
    const Json &params = request.get("params");

    if (method == "fem/solve") {
        WriteResult(id, RunSolve(params, true));
        return;
    }
    if (method == "fem/validate") {
        fem::Model model;
        std::string error;
        Json result;
        if (!fem::ModelFromJson(params.dump(), &model, &error) || !model.Validate(&error)) {
            result["ok"] = Json(false);
            result["error"] = Json(error);
        } else {
            result["ok"] = Json(true);
            result["nodes"] = Json(static_cast<double>(model.NodeCount()));
            result["elements"] = Json(static_cast<double>(model.ElementCount()));
            result["dofs"] = Json(static_cast<double>(model.DofCount()));
        }
        WriteResult(id, result.dump());
        return;
    }
    if (method == "shutdown") {
        WriteResult(id, "null");
        return;
    }
    if (method == "exit") {
        *should_exit = true;
        return;
    }
    // A notification (no id) that we do not understand is ignored, per
    // JSON-RPC; a request with an id gets a proper error back.
    if (!id.is_null()) WriteError(id, -32601, "unknown method: " + method);
}

int RunStdioServer() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string buffer;
    char chunk[16384];
    bool should_exit = false;
    while (!should_exit) {
        const std::size_t read_bytes = std::fread(chunk, 1, sizeof(chunk), stdin);
        if (read_bytes == 0) break;  // EOF: the editor went away
        buffer.append(chunk, read_bytes);
        if (!PumpRpcFrames(buffer, [&should_exit](const std::string &message) {
                HandleMessage(message, &should_exit);
            })) {
            std::fprintf(stderr, "mep-fem: framing error on stdin, exiting\n");
            return 1;
        }
    }
    return 0;
}

int RunCommandLineSolve(const char *model_path) {
    std::FILE *file = std::fopen(model_path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "mep-fem: cannot open '%s'\n", model_path);
        return 1;
    }
    std::string text;
    char chunk[16384];
    std::size_t read_bytes = 0;
    while ((read_bytes = std::fread(chunk, 1, sizeof(chunk), file)) > 0) text.append(chunk, read_bytes);
    std::fclose(file);

    Json params;
    if (!Json::Parse(text, &params)) {
        std::fprintf(stderr, "mep-fem: '%s' is not valid JSON\n", model_path);
        return 1;
    }
    const std::string result = RunSolve(params, false);
    std::printf("%s\n", result.c_str());
    // Exit status reflects the solve, so a CI script can branch on it
    // without parsing the output.
    Json parsed;
    if (Json::Parse(result, &parsed) && parsed.get("ok").as_bool()) return 0;
    return 1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--help") == 0) {
        std::printf(
            "mep-fem -- mep's finite-element solver\n"
            "\n"
            "  mep-fem                 speak JSON-RPC 2.0 over stdio (how the editor runs it)\n"
            "  mep-fem --solve <file>  solve one model given as JSON and print the result\n"
            "  mep-fem --help          this text\n");
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--solve") == 0) return RunCommandLineSolve(argv[2]);
    return RunStdioServer();
}

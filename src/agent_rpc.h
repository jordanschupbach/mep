#ifndef MEP_AGENT_RPC_H
#define MEP_AGENT_RPC_H

#include <string>
#include <vector>

class Editor;

// External AI-agent control channel: a Unix domain socket, one per running
// native mep instance, speaking Content-Length-framed JSON-RPC 2.0 (see
// rpc_framing.h) -- lets an external process (an MCP server, a script, a
// human with `socat`) drive an *already open* editor window: move the
// cursor, edit buffers, split/focus panes, run any `:` ex-command. Every
// RPC method is a thin wrapper around an Editor method that already
// exists for the embedded-Lua `mep.*` API (SetCursorForLua, RunCommand,
// ...) -- this module adds no new editor behavior, only a second way to
// reach the same primitives from outside the process.
//
// Threading: a background thread owns accept()/read() per connection and
// only ever *parses* bytes into JSON-RPC request objects, queuing them for
// the main thread -- it never touches Editor. PollOnce (called once per
// frame, same as JobManager::Instance().PollAll()) drains that queue and
// dispatches into Editor on the main thread, matching this codebase's
// existing rule (see job.h's own doc comment) that Editor -- a single
// global with no locking anywhere -- is only ever mutated from the main
// thread, once per frame.
//
// Unavailable on the wasm/Emscripten build (no Unix sockets or threads in
// a browser sandbox) and on Windows for now (no AF_UNIX story wired up
// here yet) -- every function below is a no-op there, so callers need no
// #ifdef of their own.
namespace mep::agent {

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)

// Binds this instance's socket (`<MepAgentSocketDir()>/<pid>.sock`) and
// starts its accept thread. Safe to call once at startup; a bind/listen
// failure just leaves the feature unavailable (logged to stderr, not
// fatal -- matches how a missing external tool degrades elsewhere in this
// codebase, e.g. Job::SpawnFailed).
/**
 * @brief Binds this instance's agent-control Unix socket and starts its accept thread.
 */
void Start();

// Closes the listener, joins every connection's reader thread, and
// unlinks the socket file. Called once from main()'s existing shutdown
// path, before the process exits.
/**
 * @brief Shuts down the agent-control socket: stops accepting, closes every connection, and unlinks the socket file.
 */
void Stop();

// Drains every connection's parsed requests, dispatches each against
// `editor`, and writes back its JSON-RPC response -- all on the calling
// (main) thread. Call once per frame, alongside JobManager::Instance().
// PollAll(). A no-op if Start() was never called or binding failed.
/**
 * @brief Dispatches every connected agent's pending JSON-RPC requests against the editor and sends their responses; call once per frame.
 * @param editor The editor instance to dispatch RPC requests against.
 */
void PollOnce(Editor &editor);

// Absolute path of the bound socket, or "" if Start() hasn't run yet or
// binding failed. Used by main()'s startup log line and the :AgentSocket
// ex-command.
/**
 * @brief Returns the absolute path of the bound agent-control socket.
 * @return The socket path, or "" if Start() hasn't run yet or binding failed.
 */
std::string SocketPath();

// One connected agent's identity + its own independent virtual cursor
// (COLLAB_CURSORS_PLAN.md) -- a plain snapshot struct, not a reference
// into agent_rpc.cpp's internal connection state (which is mutex-
// guarded and only safe to touch from within that file). `Editor::
// Participants()` (editor.h) wraps this call to merge it with human
// collaborators for rendering; this type is deliberately kept separate
// from Editor::ParticipantInfo (same shape, different type) so this
// header keeps its existing forward-declare-only discipline (no
// editor.h include here).
struct AgentParticipant {
    std::string id;
    std::string name;
    int buffer_id = -1;
    int row = 0, col = 0;
    bool has_location = false;
    // "" (never reported) | "idle" | "thinking" | "writing" |
    // "awaiting_input" | "done" -- see session.setStatus in agent_rpc.cpp
    // and Connection::status's own comment for how this gets set.
    // Rendered as a small badge on the agent's tab-bar chip
    // (COLLAB_CURSORS_PLAN.md Phase 1g, main.cpp's DrawAgentStatusBadge);
    // "" renders no badge at all, same as an agent predating this field.
    std::string status;
    // The `:terminal` buffer this agent is running inside, or -1 if it
    // didn't say (an agent started outside mep, or one predating this
    // field). Reported by the agent itself in session.identify
    // (mcp/server.ts reads it from $MEP_TERMINAL_BUFFER, which
    // Editor::TerminalSpawn exports into every terminal it opens) -- what
    // the AI-agents sidebar (kBuiltinAiTerminal, main.cpp) uses to pair a
    // connected agent with the pane/tab/workspace to jump to.
    int terminal_buffer_id = -1;
};
// Snapshots every currently-connected agent's identity/cursor/status.
// Safe to call from the main thread at any time (including mid-PollOnce,
// e.g. from Editor::Participants() while rendering).
/**
 * @brief Snapshots every currently-connected agent's identity, cursor, and status.
 * @return One AgentParticipant per currently-connected agent connection.
 */
std::vector<AgentParticipant> AgentParticipants();

#else

/**
 * @brief No-op stand-in for Start() on platforms without the agent-control socket (wasm/Windows).
 */
inline void Start() {}
/**
 * @brief No-op stand-in for Stop() on platforms without the agent-control socket (wasm/Windows).
 */
inline void Stop() {}
/**
 * @brief No-op stand-in for PollOnce() on platforms without the agent-control socket (wasm/Windows).
 * @param editor Unused.
 */
inline void PollOnce(Editor &) {}
/**
 * @brief No-op stand-in for SocketPath() on platforms without the agent-control socket (wasm/Windows).
 * @return Always "".
 */
inline std::string SocketPath() { return ""; }

struct AgentParticipant {
    std::string id;
    std::string name;
    int buffer_id = -1;
    int row = 0, col = 0;
    bool has_location = false;
    std::string status;
};
/**
 * @brief No-op stand-in for AgentParticipants() on platforms without the agent-control socket (wasm/Windows).
 * @return Always empty.
 */
inline std::vector<AgentParticipant> AgentParticipants() { return {}; }

#endif

}  // namespace mep::agent

#endif  // MEP_AGENT_RPC_H

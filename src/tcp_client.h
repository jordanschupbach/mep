#ifndef MEP_TCP_CLIENT_H
#define MEP_TCP_CLIENT_H

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Minimal async TCP client, added for DAP adapters that speak JSON-RPC
// over a plain TCP socket instead of stdio the way lldb-dap/debugpy do
// (R's vscDebugger is the motivating case: it starts an R process that
// opens a TCP listener, rather than exposing a spawn-and-talk-over-stdio
// binary). Mirrors job.h/JobManager's shape and threading invariant on
// purpose: a background reader thread per connection queues raw bytes,
// and TcpJsonRpcManager::PollAll() (called once per frame, right next to
// JobManager::Instance().PollAll()) is the only place a caller-supplied
// callback ever runs, so Editor/Lua state can be touched from it without
// extra synchronization -- exactly like every other async I/O source in
// this codebase.
//
// Unsupported on the wasm/emscripten build (no raw sockets in a browser
// sandbox) and on Windows (no POSIX sockets here) -- Connect() fails
// gracefully there, the same way Job::Spawn() already degrades when an
// external tool is missing.
class TcpConnection {
public:
    /**
     * @brief Opens a TCP connection to host:port (non-blocking connect with a
     * short timeout so a dead/unreachable port can't hang the caller) and,
     * on success, starts a background reader thread streaming raw bytes.
     * @param host Hostname or IP address to connect to.
     * @param port TCP port to connect to.
     */
    TcpConnection(const std::string &host, int port);
    /**
     * @brief Closes the socket (if still open) and joins the reader thread.
     */
    ~TcpConnection();

    TcpConnection(const TcpConnection &) = delete;
    TcpConnection &operator=(const TcpConnection &) = delete;

    /**
     * @brief Reports whether the connection attempt itself failed (bad host,
     * refused, or timed out) -- the socket never came up at all.
     * @return True if the connection never succeeded.
     */
    bool ConnectFailed() const { return connect_failed_; }
    /**
     * @brief Reports whether the connection is closed (failed, ended by the
     * peer, or Close() was called and the reader thread observed EOF).
     * @return True once no more data will ever arrive.
     */
    bool Finished() const { return finished_.load(); }

    /**
     * @brief Writes to the socket (e.g. a framed JSON-RPC request).
     * @param data Bytes to write; written in full via a blocking write loop.
     * @return False if the socket isn't open.
     */
    bool Send(const std::string &data);
    /**
     * @brief Shuts down and closes the socket, signalling the reader thread to stop.
     */
    void Close();

    /**
     * @brief Drains everything queued since the last call, in arrival order.
     * Only ever called from the main thread (TcpJsonRpcManager::PollAll).
     * @return The raw byte chunks received since the last drain.
     */
    std::vector<std::string> DrainRaw();

private:
    int fd_ = -1;
    std::thread reader_thread_;
    std::mutex mu_;
    std::deque<std::string> pending_;
    std::atomic<bool> finished_{false};
    bool connect_failed_ = false;

    /**
     * @brief Background-thread loop that blocks on recv() and queues each
     * chunk read until the peer closes or an unrecoverable error occurs.
     */
    void ReaderLoop();
};

// Client ids handed out by TcpJsonRpcManager start here, a range disjoint
// from JobManager's (which starts at 1 and grows by 1 per spawn) -- so
// lua_env.cpp's mep.lsp_request/notify/stop/is_running can tell "is this
// client backed by a spawned process or a raw TCP socket" from the id
// alone and route to the right manager, while the JSON-RPC framing/
// dispatch code (DispatchLspMessage, LspClientState) stays completely
// transport-agnostic.
constexpr int kTcpClientIdBase = 1 << 20;

// Owns every live TcpConnection, polled once per frame from the main loop.
class TcpJsonRpcManager {
public:
    /**
     * @brief Returns the process-wide TcpJsonRpcManager singleton, constructing it on first use.
     * @return Reference to the single TcpJsonRpcManager instance.
     */
    static TcpJsonRpcManager &Instance();

    struct Callbacks {
        std::function<void(const std::string &)> on_data_raw;
        std::function<void(int)> on_exit;  // always invoked with -1 (a socket has no exit code)
    };

    /**
     * @brief Opens and registers a TCP connection; returns its id (in the
     * kTcpClientIdBase+ range) immediately, even if the connection attempt
     * fails -- a failed connection's on_exit(-1) fires on the very next
     * PollAll(), not synchronously, matching JobManager::Spawn's own
     * "callbacks only run from PollAll" invariant.
     * @param host Hostname or IP address to connect to.
     * @param port TCP port to connect to.
     * @param callbacks Handlers invoked from PollAll() for this connection's data/exit.
     * @return The new connection's id.
     */
    int Connect(const std::string &host, int port, Callbacks callbacks);
    /**
     * @brief Writes to the given connection's socket.
     * @param id Id of the target connection, as returned by Connect().
     * @param data Bytes to write.
     * @return False if the connection doesn't exist or is closed.
     */
    bool Send(int id, const std::string &data);
    /**
     * @brief Closes the given connection.
     * @param id Id of the target connection, as returned by Connect().
     */
    void Close(int id);
    /**
     * @brief Reports whether the given connection is still registered and not yet finished.
     * @param id Id of the target connection, as returned by Connect().
     * @return True if the connection exists and hasn't finished.
     */
    bool IsRunning(int id) const;

    /**
     * @brief Call once per frame: drains every live connection's buffered
     * data/exit status and invokes its stored callbacks, then reaps
     * finished connections whose exit callback has already fired.
     */
    void PollAll();

    /**
     * @brief Closes every still-open connection. Called once from main()
     * right after the render loop exits, mirroring JobManager::ShutdownAll.
     */
    void ShutdownAll();

private:
    struct Entry {
        int id = 0;
        std::shared_ptr<TcpConnection> conn;
        Callbacks callbacks;
        bool exit_reported = false;
    };
    std::vector<Entry> conns_;
    int next_id_ = kTcpClientIdBase;

    /**
     * @brief Looks up a registered connection by id.
     * @param id Id of the connection to find, as returned by Connect().
     * @return Pointer to the matching TcpConnection, or nullptr if no live entry has that id.
     */
    TcpConnection *Find(int id);
};

#endif  // MEP_TCP_CLIENT_H

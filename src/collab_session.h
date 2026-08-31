#pragma once

#include "collab_crdt.h"

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__EMSCRIPTEN__)
/**
 * @brief Delivers one text message received on the browser WebSocket to the CollabSession that owns `handle`.
 * @param handle Handle identifying the owning CollabSession, as registered in Start().
 * @param text Heap-allocated UTF-8 message text (freed by this function); treated as empty if null.
 */
extern "C" void mep_collab_web_message(int handle, char *text);
/**
 * @brief Updates the connection state of the CollabSession identified by `handle` in response to a browser WebSocket event.
 * @param handle Handle identifying the owning CollabSession, as registered in Start().
 * @param state Event code: 1 for open, 0 for close, negative for an error.
 */
extern "C" void mep_collab_web_event(int handle, int state);
#endif

namespace mep::collab {

struct Collaborator {
    std::string id;
    std::string name;
    int row = 0;
    int col = 0;
    bool has_location = false;
};

// Threaded client for one capability-link session. All CRDT mutation happens
// on the editor thread; the worker only handles the blocking socket and queues
// validated operations/messages for Synchronize().
class CollabSession {
public:
    /**
     * @brief Constructs a session for one capability-link room and seeds the local CRDT with the buffer's current text.
     * @param url The ws:// or wss:// capability-link URL of the collaboration room.
     * @param name Display name to announce to peers; replaced with "Anonymous" if empty.
     * @param initial_text The editor buffer's current contents, inserted into the CRDT as the starting state.
     */
    CollabSession(std::string url, std::string name, const std::string &initial_text);
    /**
     * @brief Stops the session and releases its resources.
     */
    ~CollabSession();
    /**
     * @brief Disabled: a CollabSession owns a worker thread and socket and cannot be copy-constructed.
     */
    CollabSession(const CollabSession &) = delete;
    /**
     * @brief Disabled: a CollabSession owns a worker thread and socket and cannot be copy-assigned.
     */
    CollabSession &operator=(const CollabSession &) = delete;
    /**
     * @brief Disabled: always held behind std::unique_ptr (see Editor::collaboration_); never moved by value.
     */
    CollabSession(CollabSession &&) = delete;
    /**
     * @brief Disabled: always held behind std::unique_ptr (see Editor::collaboration_); never moved by value.
     */
    CollabSession &operator=(CollabSession &&) = delete;

    /**
     * @brief Begins connecting to the relay, running the socket work on a background thread (or, on Emscripten, an asynchronous browser WebSocket).
     */
    void Start();
    /**
     * @brief Signals the session to stop, closes the socket, and waits for the worker thread to finish (if any).
     */
    void Stop();
    /**
     * @brief Reports whether the session currently has a live connection to the relay.
     * @return True if connected to the relay, false otherwise.
     */
    bool connected() const;
    /**
     * @brief Retrieves the most recent connection error, if any.
     * @return The last recorded error message, or an empty string if none occurred.
     */
    std::string error() const;
    /**
     * @brief Returns the capability-link URL this session was constructed with.
     * @return The session's ws:// or wss:// URL.
     */
    const std::string &url() const { return url_; }

    /**
     * @brief Reconciles an editor buffer with the CRDT. `local_text` is interpreted as one local splice since the
     * previous call: it is diffed against the previously synchronized text, the resulting edit is applied to the
     * local CRDT and sent to peers, and any operations received from peers are applied and merged in.
     * @param local_text The editor buffer's current text, reflecting at most one local edit since the last call.
     * @param merged_text Out parameter set to the fully merged text when remote changes were applied.
     * @return True if remote changes were applied and `merged_text` was set, false otherwise.
     */
    bool Synchronize(const std::string &local_text, std::string *merged_text);
    /**
     * @brief Sends the local cursor position to peers, but only when it has changed since the last call and the session is connected.
     * @param row Zero-based cursor row.
     * @param col Zero-based cursor column.
     */
    void SetPresence(int row, int col);
    /**
     * @brief Returns a snapshot of the other participants currently known to this session.
     * @return A vector of the currently known collaborators.
     */
    std::vector<Collaborator> Collaborators() const;

private:
#if defined(__EMSCRIPTEN__)
    friend void ::mep_collab_web_message(int handle, char *text);
    friend void ::mep_collab_web_event(int handle, int state);
#endif
    std::string url_, name_;
    TextCrdt document_;
    std::string last_text_;
#if defined(__EMSCRIPTEN__)
    int web_handle_ = 0;
#else
    std::thread worker_;
#endif
    bool stopping_ = false;
    bool connected_ = false;
    bool announced_ = false;
    int last_presence_row_ = -1;
    int last_presence_col_ = -1;
    std::string error_;
    mutable std::mutex mutex_;
    std::vector<CrdtOperation> received_operations_;
    bool received_sync_request_ = false;
    std::map<std::string, Collaborator> collaborators_;

#if !defined(__EMSCRIPTEN__)
    /**
     * @brief Worker-thread entry point: connects the socket, announces presence, receives messages until the
     * connection ends, and records the connected state and any error.
     */
    void Run();
    std::mutex send_mutex_;
    class SocketHolder;
    SocketHolder *socket_ = nullptr;
#endif
    /**
     * @brief Parses one incoming relay message and applies its effect (queued CRDT operations, a sync request, or a
     * collaborator roster/presence update) under the session's mutex.
     * @param text The raw JSON message text received from the relay.
     */
    void ReceiveMessage(const std::string &text);
    /**
     * @brief Serializes and sends a JSON message over the socket, if connected.
     * @param message The JSON message to send.
     */
    void Send(const Json &message);
};

}  // namespace mep::collab

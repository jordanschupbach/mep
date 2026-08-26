#pragma once

#include "collab_crdt.h"

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__EMSCRIPTEN__)
extern "C" void mep_collab_web_message(int handle, char *text);
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
    CollabSession(std::string url, std::string name, const std::string &initial_text);
    ~CollabSession();
    CollabSession(const CollabSession &) = delete;
    CollabSession &operator=(const CollabSession &) = delete;

    void Start();
    void Stop();
    bool connected() const;
    std::string error() const;
    const std::string &url() const { return url_; }

    // Reconciles an editor buffer with the CRDT. `local_text` is interpreted
    // as one local splice since the previous call. On remote changes, returns
    // true and stores the fully merged text in `merged_text`.
    bool Synchronize(const std::string &local_text, std::string *merged_text);
    void SetPresence(int row, int col);
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
    void Run();
    std::mutex send_mutex_;
    class SocketHolder;
    SocketHolder *socket_ = nullptr;
#endif
    void ReceiveMessage(const std::string &text);
    void Send(const Json &message);
};

}  // namespace mep::collab

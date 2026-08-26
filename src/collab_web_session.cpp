#include "collab_session.h"

#include "json.h"

#include <algorithm>
#include <cstdlib>
#include <map>

#include <emscripten/emscripten.h>

namespace mep::collab {
namespace {
std::map<int, CollabSession *> &Sessions() { static std::map<int, CollabSession *> sessions; return sessions; }
int &NextHandle() { static int handle = 1; return handle; }
std::string EncodeName(const std::string &name) {
    static constexpr char hex[] = "0123456789ABCDEF"; std::string result;
    for (unsigned char c : name) { if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') result.push_back(static_cast<char>(c)); else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; } }
    return result;
}
Json OperationsMessage(const char *type, const std::vector<CrdtOperation> &operations) {
    Json message = Json::Object(); message["type"] = type; Json list = Json::Array(); for (const auto &op : operations) list.push_back(CrdtOperationToJson(op)); message["ops"] = std::move(list); return message;
}
std::vector<CrdtOperation> StateOperations(const TextCrdt &document) {
    std::vector<CrdtOperation> all; const Json snapshot = document.Snapshot();
    for (const Json &entry : snapshot.get("nodes").items()) { CrdtOperation op; if (!CrdtOperationFromJson(entry, &op)) continue; all.push_back(op); if (entry.get("deleted").as_bool()) all.push_back({CrdtOperation::Kind::Delete, op.id, {}, 0}); }
    return all;
}
}

EM_JS(void, mep_collab_web_connect, (int handle, const char *url), {
    if (!Module.mepCollabSockets) Module.mepCollabSockets = {};
    try {
        const socket = new WebSocket(UTF8ToString(url)); Module.mepCollabSockets[handle] = socket;
        socket.onopen = () => Module._mep_collab_web_event(handle, 1);
        socket.onmessage = event => { if (typeof event.data === 'string') { const p = stringToNewUTF8(event.data); Module._mep_collab_web_message(handle, p); _free(p); } };
        socket.onerror = () => Module._mep_collab_web_event(handle, -1);
        socket.onclose = () => { delete Module.mepCollabSockets[handle]; Module._mep_collab_web_event(handle, 0); };
    } catch (_) { Module._mep_collab_web_event(handle, -1); }
});
EM_JS(void, mep_collab_web_send, (int handle, const char *text), {
    const socket = Module.mepCollabSockets && Module.mepCollabSockets[handle]; if (socket && socket.readyState === WebSocket.OPEN) socket.send(UTF8ToString(text));
});
EM_JS(void, mep_collab_web_close, (int handle), {
    const socket = Module.mepCollabSockets && Module.mepCollabSockets[handle]; if (socket) socket.close();
});

extern "C" EMSCRIPTEN_KEEPALIVE void mep_collab_web_message(int handle, char *text) {
    auto it = Sessions().find(handle); if (it != Sessions().end()) it->second->ReceiveMessage(text ? text : ""); std::free(text);
}
extern "C" EMSCRIPTEN_KEEPALIVE void mep_collab_web_event(int handle, int state) {
    auto it = Sessions().find(handle); if (it == Sessions().end()) return;
    std::lock_guard<std::mutex> lock(it->second->mutex_);
    it->second->connected_ = state == 1;
    if (state < 0) it->second->error_ = "browser WebSocket connection failed";
    else if (state == 0 && !it->second->stopping_) it->second->error_ = "relay disconnected";
}

CollabSession::CollabSession(std::string url, std::string name, const std::string &initial_text)
    : url_(std::move(url)), name_(name.empty() ? "Anonymous" : std::move(name)), document_(), last_text_(initial_text) { document_.Insert(0, initial_text); }
CollabSession::~CollabSession() { Stop(); }
void CollabSession::Start() {
    web_handle_ = NextHandle()++; Sessions()[web_handle_] = this;
    const std::string url = url_ + (url_.find('?') == std::string::npos ? "?name=" : "&name=") + EncodeName(name_);
    mep_collab_web_connect(web_handle_, url.c_str());
}
void CollabSession::Stop() { if (!web_handle_) return; { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; connected_ = false; } mep_collab_web_close(web_handle_); Sessions().erase(web_handle_); web_handle_ = 0; }
bool CollabSession::connected() const { std::lock_guard<std::mutex> lock(mutex_); return connected_; }
std::string CollabSession::error() const { std::lock_guard<std::mutex> lock(mutex_); return error_; }
void CollabSession::Send(const Json &message) { if (web_handle_) { const std::string text = message.dump(); mep_collab_web_send(web_handle_, text.c_str()); } }
void CollabSession::ReceiveMessage(const std::string &text) {
    Json message; if (!Json::Parse(text, &message) || !message.is_object()) return;
    const std::string type = message.get("type").as_string(), peer = message.get("peer").as_string(); std::lock_guard<std::mutex> lock(mutex_);
    if (type == "update" || type == "sync_response") { const Json &ops = message.get("ops"); if (!ops.is_array()) return; for (const Json &entry : ops.items()) { CrdtOperation op; if (CrdtOperationFromJson(entry, &op)) received_operations_.push_back(op); } }
    else if (type == "sync_request") received_sync_request_ = true;
    else if (type == "welcome") { const Json &peers = message.get("peers"); if (peers.is_array()) for (const Json &item : peers.items()) { const std::string id = item.get("peer").as_string(); if (!id.empty()) collaborators_[id] = {id, item.get("name").as_string(), 0, 0, false}; } }
    else if (type == "peer_join" || type == "presence") { if (peer.empty()) return; Collaborator &other = collaborators_[peer]; other.id = peer; other.name = message.get("name").as_string(other.name); if (type == "presence") { other.row = message.get("row").as_int(); other.col = message.get("col").as_int(); other.has_location = true; } }
    else if (type == "peer_leave") collaborators_.erase(peer);
}
bool CollabSession::Synchronize(const std::string &local_text, std::string *merged_text) {
    std::vector<CrdtOperation> remote; bool answer_sync = false; { std::lock_guard<std::mutex> lock(mutex_); remote.swap(received_operations_); answer_sync = received_sync_request_; received_sync_request_ = false; }
    std::vector<CrdtOperation> local;
    if (local_text != last_text_) { size_t prefix = 0; while (prefix < local_text.size() && prefix < last_text_.size() && local_text[prefix] == last_text_[prefix]) ++prefix; size_t local_tail = local_text.size(), old_tail = last_text_.size(); while (local_tail > prefix && old_tail > prefix && local_text[local_tail - 1] == last_text_[old_tail - 1]) { --local_tail; --old_tail; } auto erased = document_.Erase(prefix, old_tail - prefix); local.insert(local.end(), erased.begin(), erased.end()); auto inserted = document_.Insert(prefix, local_text.substr(prefix, local_tail - prefix)); local.insert(local.end(), inserted.begin(), inserted.end()); }
    if (!remote.empty()) document_.Apply(remote); if (!announced_ && connected()) { Send(OperationsMessage("update", StateOperations(document_))); announced_ = true; } if (answer_sync) Send(OperationsMessage("sync_response", StateOperations(document_))); if (!local.empty()) Send(OperationsMessage("update", local));
    const std::string current = document_.Text(); const bool changed = current != local_text; last_text_ = current; if (changed && merged_text) *merged_text = current; return changed;
}
void CollabSession::SetPresence(int row, int col) { if (!connected() || (row == last_presence_row_ && col == last_presence_col_)) return; last_presence_row_ = row; last_presence_col_ = col; Json message = Json::Object(); message["type"] = "presence"; message["row"] = row; message["col"] = col; Send(message); }
std::vector<Collaborator> CollabSession::Collaborators() const { std::lock_guard<std::mutex> lock(mutex_); std::vector<Collaborator> result; for (const auto &[id, collaborator] : collaborators_) result.push_back(collaborator); return result; }
}  // namespace mep::collab

#ifndef MEP_MAIL_UI_H
#define MEP_MAIL_UI_H

// State behind the :Mail pane (editor_mail.cpp). The pane is an ordinary,
// read-only text buffer whose lines are regenerated from this state, so
// every Vim motion, search, count and mark works on it unchanged; the
// mail commands are single keys layered on top (MailUi::HandleKey).
//
// One mail buffer per workspace, navigated like a file manager:
//
//   Folders  --Enter-->  List  --Enter-->  Message
//            <---- h/- ---     <---- q/- ---
//
// Compose buffers are separate: real draft files under
// MailDataDir()/drafts, edited as plain text and sent with :MailSend.

#include "mail.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct MailRow {
    enum class Kind { None, Account, Folder, Draft, Message, LoadMore, Attachment };
    Kind kind = Kind::None;
    int account = -1;
    int index = -1;    // folder / message / attachment index
    std::string path;  // draft file
};

struct MailListState {
    std::string mailbox;  // raw (modified UTF-7) name
    std::vector<mail::MessageSummary> msgs;  // newest first
    uint32_t exists = 0;
    uint32_t min_seq = 0;  // lowest sequence number loaded (0 = none yet)
    bool loaded = false;
    std::string search;    // non-empty: these are search results for it
};

struct MailSession {
    enum class View { Folders, List, Message };
    int buffer_id = -1;
    View view = View::List;
    int account = 0;
    MailListState list;
    // The open message (View::Message).
    uint32_t msg_uid = 0;
    mail::Message msg;
    bool msg_loaded = false;
    // Folders view used as a destination picker for `M`.
    bool picking = false;
    std::vector<uint32_t> pick_uids;
    // In-flight work: `generation` invalidates replies to a superseded
    // load (a newer folder/message was asked for meanwhile).
    int generation = 0;
    int loading = 0;  // count of view-changing loads in flight
    std::vector<int> jobs;
    std::string status;  // error/progress, shown after the title
    std::string title;        // the pane header's text (Editor::MailPaneTitle)
    std::string short_title;  // tab strip / status line (Editor::SpecialBufferName)
    double last_refresh = 0.0;
    // Where the cursor was in each view, restored on the way back.
    int folders_row = 0, list_row = 0;
    // A first `d` waiting for its second (dd), as in Vim.
    bool pending_d = false;
    int pending_d_count = 0;
    std::vector<MailRow> rows;  // what each buffer row is
};

// Context for a draft opened from the mail pane (reply target, where to
// return after sending), keyed by the draft's absolute path.
struct MailDraftContext {
    std::string account_id;
    std::string mailbox;
    uint32_t reply_uid = 0;
    int return_buffer = -1;
    std::vector<std::string> temp_files;  // forwarded attachments to clean up
};

struct MailState {
    bool accounts_loaded = false;
    long long accounts_stamp = -1;  // the accounts file's mtime when last read
    std::vector<mail::Account> accounts;
    std::string accounts_error;
    std::unordered_map<std::string, std::string> secrets;               // account id -> password/token
    std::unordered_map<std::string, std::vector<mail::Folder>> folders;  // account id -> LIST
    std::unordered_map<int, MailSession> sessions;                      // buffer id -> pane state
    std::unordered_map<std::string, MailDraftContext> drafts;          // draft path -> context
    int ns = -1;
    bool sending = false;
    bool signing_in = false;  // a browser sign-in is waiting for its redirect
    int signin_job = 0;       // its job, killed when a new sign-in starts
    bool unlocking = false;   // the vault's key is being derived
};

#endif  // MEP_MAIL_UI_H

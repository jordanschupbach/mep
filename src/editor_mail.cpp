// :Mail -- the email client's pane. The protocol and format work lives in
// mail.h (pure) and mail_net.h (curl, blocking); this file is the editor
// side: the mail buffer's views (folders, message list, message), its
// single-key commands, compose buffers and :MailSend, and the background
// jobs that keep IMAP/SMTP off the frame loop.
//
// The pane is a plain text buffer regenerated from MailSession whenever
// the state changes, so Vim's own motions, counts, search, marks and
// yanks all work on it; MailUi::HandleKey only claims the mail keys (and
// swallows the editing ones, since the text is generated). See
// help/mail.org for the user-facing tour.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "editor.h"
#include "frame_activity.h"
#include "gfx/platform.h"
#include "job.h"
#include "mail.h"
#include "mail_net.h"
#include "mail_ui.h"

namespace {

constexpr int kFirstRow = 0;              // the title lives in the pane header, not the text
constexpr uint32_t kPageSize = 100;       // messages per load / "load more"
constexpr size_t kSearchLimit = 200;      // newest search hits shown
constexpr double kAutoRefreshSec = 180.0;  // a visible list re-syncs this often

std::string HomeDir() {
    const char *home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    return home && *home ? std::string(home) : std::string();
}

std::string ExpandPath(const std::string &p_in) {
    std::string p = mail::Trim(p_in);
    if (p.size() >= 2 && ((p.front() == '"' && p.back() == '"') || (p.front() == '\'' && p.back() == '\''))) {
        p = p.substr(1, p.size() - 2);
    }
    if (!p.empty() && p[0] == '~') p = HomeDir() + p.substr(1);
    std::error_code ec;
    std::filesystem::path path(p);
    if (path.is_relative()) path = std::filesystem::current_path(ec) / path;
    return path.lexically_normal().string();
}

std::string DraftsDir() {
    const std::string base = mail::MailDataDir();
    if (base.empty()) return "";
    std::error_code ec;
    std::filesystem::create_directories(base + "/drafts", ec);
    return base + "/drafts";
}

std::string DownloadsDir() {
    std::string dir = HomeDir();
    if (dir.empty()) return ".";
    std::error_code ec;
    if (std::filesystem::is_directory(dir + "/Downloads", ec)) return dir + "/Downloads";
    return dir;
}

std::string UniquePath(const std::string &dir, const std::string &name) {
    std::error_code ec;
    std::filesystem::path base(name);
    const std::string stem = base.stem().string();
    const std::string ext = base.extension().string();
    std::string candidate = dir + "/" + name;
    for (int n = 1; std::filesystem::exists(candidate, ec) && n < 1000; n++) {
        candidate = dir + "/" + stem + " (" + std::to_string(n) + ")" + ext;
    }
    return candidate;
}

bool WriteFileBytes(const std::string &path, const std::string &data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << data;
    return static_cast<bool>(out);
}

bool ReadFileBytes(const std::string &path, std::string *data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *data = ss.str();
    return true;
}

// Text for the buffer: CR dropped, tabs expanded, control characters shown as spaces.
std::vector<std::string> TextLines(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    size_t col = 0;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
            col = 0;
            continue;
        }
        if (c == '\t') {
            const size_t n = 4 - col % 4;
            cur.append(n, ' ');
            col += n;
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
        cur.push_back(c);
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) col++;
    }
    out.push_back(cur);
    return out;
}

std::string JoinDraftLines(const std::vector<std::string> &lines) {
    std::string s;
    for (const std::string &l : lines) s += l + "\n";
    return s;
}

// Drafts are keyed by absolute, normalised path (what DraftKey derives
// from an open buffer), whatever form the data directory came in.
std::string AbsKey(const std::string &path) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    return (ec ? std::filesystem::path(path) : p).lexically_normal().string();
}

std::string RandomSuffix() {
    std::random_device rd;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06x", rd() & 0xFFFFFFu);
    return buf;
}

std::string Plural(size_t n, const char *word) {
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

bool ViewableInMep(const std::string &filename) {
    static const char *kExt[] = {".pdf", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", ".txt", ".md",
                                 ".org", ".csv", ".json", ".xml", ".docx", ".xlsx", ".pptx", ".odt", ".ods",
                                 ".odp", ".rtf", ".mp3", ".wav", ".flac", ".mp4", ".mov", ".eml", ".ics",
                                 ".patch", ".diff", ".log", ".py", ".c", ".cpp", ".h", ".js", ".ts", ".tex"};
    const std::string lower = mail::Lower(filename);
    for (const char *e : kExt) {
        const size_t n = std::strlen(e);
        if (lower.size() > n && lower.compare(lower.size() - n, n, e) == 0) return true;
    }
    return false;
}

// What a worker needs to talk to the server: the account, its secret if
// already known (else the worker looks it up), and how that went.
struct Cred {
    mail::Account account;
    std::string secret;
    bool resolved_here = false;
    std::string error;
    bool Ensure() {
        if (!secret.empty()) return true;
        secret = mail::ResolveSecret(account, &error);
        resolved_here = !secret.empty();
        return resolved_here;
    }
    mail::Session Session() const { return {account, secret}; }
};

std::string FindRoleFolder(const mail::Account &a, const std::vector<mail::Folder> &folders, const std::string &role) {
    if (role == "trash" && !a.trash.empty()) return a.trash;
    if (role == "archive" && !a.archive.empty()) return a.archive;
    if (role == "sent" && !a.sent.empty()) return a.sent;
    if (role == "drafts" && !a.drafts.empty()) return a.drafts;
    for (const mail::Folder &f : folders) {
        if (f.role == role) return f.raw;
    }
    // Gmail's archive is "remove the Inbox label", i.e. a move to All Mail.
    if (role == "archive") {
        for (const mail::Folder &f : folders) {
            if (f.role == "all") return f.raw;
        }
    }
    return "";
}

const char kHelpText[] =
    "Mail pane -- ordinary Vim motions work everywhere (j/k, gg/G, Ctrl-d/u,\n"
    "/search, n/N, counts, marks, v/y to copy text). Mail keys:\n"
    "\n"
    "Folders\n"
    "  Enter / l      open the folder (or draft) under the cursor\n"
    "  $              reload the folder lists\n"
    "  q              close the mail pane\n"
    "\n"
    "Message list\n"
    "  Enter / l / o  read the message\n"
    "  h / - / q      back to the folders\n"
    "  c / m          compose a new message\n"
    "  r / R / f      reply / reply to all / forward\n"
    "  dd / x         delete (to Trash; in Trash: for good) -- 3dd deletes three\n"
    "  a              archive          M  move to a folder (pick it, Enter)\n"
    "  s              star / flag      U  mark read / unread\n"
    "  $              refresh          +  load older messages\n"
    "  S              search on the server (Gmail: Gmail's search syntax)\n"
    "  A              next account\n"
    "\n"
    "Message\n"
    "  q / -          back to the list\n"
    "  J / K          next / previous message\n"
    "  r R f dd x a M s U   as in the list\n"
    "  Enter          on an Attach: line -- save it to Downloads and open it\n"
    "\n"
    "Composing (a plain text buffer: header lines, a blank line, the body)\n"
    "  :MailSend               send it        :w saves the draft\n"
    "  :MailAttach <file>      attach a file  :MailDiscard throws it away\n"
    "\n"
    ":Mail [account] [folder]  :MailCompose [to]  :MailSetup (add an account)";

}  // namespace

struct MailUi {
    // --- State helpers ---------------------------------------------------

    static long long FileStamp(const std::string &path) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path, ec);
        return ec ? -1 : static_cast<long long>(t.time_since_epoch().count());
    }

    // Reads the accounts file -- again whenever it changed on disk since,
    // so a hand edit (or a fixed typo) takes effect at the next :Mail.
    static void EnsureAccounts(Editor &e) {
        MailState &m = e.mail_;
        if (m.ns < 0) m.ns = e.CreateNamespace("mail");
        const std::string path = mail::AccountsPath();
        const long long stamp = path.empty() ? -1 : FileStamp(path);
        if (m.accounts_loaded && stamp == m.accounts_stamp) return;
        m.accounts_loaded = true;
        m.accounts_stamp = stamp;
        m.accounts_error.clear();
        if (path.empty()) return;
        m.accounts = mail::LoadAccounts(path, &m.accounts_error);
        for (auto &kv : m.sessions) {
            if (kv.second.account >= static_cast<int>(m.accounts.size())) kv.second.account = 0;
        }
    }

    static void SaveAccounts(Editor &e) {
        const std::string path = mail::AccountsPath();
        if (path.empty() || !mail::SaveAccounts(path, e.mail_.accounts)) {
            e.Notify("Mail: could not write the accounts file", Editor::NotifyLevel::Error);
        }
    }

    static const mail::Account *AccountAt(Editor &e, int idx) {
        if (idx < 0 || idx >= static_cast<int>(e.mail_.accounts.size())) return nullptr;
        return &e.mail_.accounts[static_cast<size_t>(idx)];
    }

    static int AccountIndexById(Editor &e, const std::string &id) {
        for (size_t i = 0; i < e.mail_.accounts.size(); i++) {
            if (e.mail_.accounts[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }

    static int AccountIndexForEmail(Editor &e, const std::string &email) {
        const std::string want = mail::Lower(mail::Trim(email));
        for (size_t i = 0; i < e.mail_.accounts.size(); i++) {
            if (mail::Lower(e.mail_.accounts[i].email) == want) return static_cast<int>(i);
        }
        return -1;
    }

    static std::shared_ptr<Cred> MakeCred(Editor &e, int account) {
        auto c = std::make_shared<Cred>();
        if (const mail::Account *a = AccountAt(e, account)) {
            c->account = *a;
            auto it = e.mail_.secrets.find(a->id);
            if (it != e.mail_.secrets.end()) c->secret = it->second;
        }
        return c;
    }

    // Main thread, after a job: remember a secret the worker looked up, and
    // forget one the server refused (so a corrected one is read next time).
    static void AfterCred(Editor &e, const Cred &c, const std::string &error) {
        if (c.resolved_here && !c.secret.empty()) e.mail_.secrets[c.account.id] = c.secret;
        if (error.find("login refused") != std::string::npos) e.mail_.secrets.erase(c.account.id);
    }

    // A failure worth a toast: not the "cancelled" of a job stopped because
    // its pane closed.
    static void ReportError(Editor &e, const std::string &error) {
        if (error.empty() || error == "cancelled") return;
        e.Notify("Mail: " + error, Editor::NotifyLevel::Error);
    }

    static MailSession *Session(Editor &e, int buffer_id) {
        auto it = e.mail_.sessions.find(buffer_id);
        return it == e.mail_.sessions.end() ? nullptr : &it->second;
    }

    static int WorkspaceMailBuffer(Editor &e) {
        for (const auto &kv : e.mail_.sessions) {
            const int id = kv.first;
            if (id < 0 || id >= static_cast<int>(e.buffers_.size())) continue;
            if (e.buffers_[static_cast<size_t>(id)].deleted) continue;
            if (!e.BufferInActiveWorkspace(id)) continue;
            return id;
        }
        return -1;
    }

    static Pane *PaneShowing(Editor &e, int buffer_id) {
        if (e.CurPane().buffer_id == buffer_id) return &e.CurPane();
        const int pane_id = e.FindPaneIdForBuffer(e.ActiveTab().root.get(), buffer_id);
        if (pane_id < 0) return nullptr;
        SplitNode *node = e.FindNode(e.ActiveTab().root.get(), pane_id);
        return node ? &node->pane : nullptr;
    }

    static int CursorRow(Editor &e, const MailSession &s) {
        Pane *p = PaneShowing(e, s.buffer_id);
        return p ? p->cursor.row : 0;
    }

    static void SetCursorRow(Editor &e, const MailSession &s, int row) {
        Pane *p = PaneShowing(e, s.buffer_id);
        if (!p) return;
        const Buffer &b = e.buffers_[static_cast<size_t>(s.buffer_id)];
        p->cursor.row = std::clamp(row, 0, std::max(0, static_cast<int>(b.lines.size()) - 1));
        p->cursor.col = 0;
        if (p->cursor.row < p->scroll_row) p->scroll_row = std::max(0, p->cursor.row - 2);
    }

    // The current pane shows `buffer_id`, with the pane's tab strip kept
    // consistent (the same bookkeeping OpenYoutubeInPlace does).
    static void ShowBufferHere(Editor &e, int buffer_id) {
        e.EnsureBufferTabSeeded(e.CurPane());
        if (!e.CurPane().buffer_tabs.empty()) {
            e.CurPane().buffer_tabs[static_cast<size_t>(e.CurPane().buffer_tab_index)] = buffer_id;
        }
        e.CurPane().buffer_id = buffer_id;
        e.CurPane().cursor = {0, 0};
        e.CurPane().scroll_row = 0;
        e.SyncModeToActivePaneBuffer();
    }

    static std::string FolderDisplayName(Editor &e, int account, const std::string &raw) {
        const mail::Account *a = AccountAt(e, account);
        if (a) {
            auto it = e.mail_.folders.find(a->id);
            if (it != e.mail_.folders.end()) {
                for (const mail::Folder &f : it->second) {
                    if (f.raw == raw) return f.name;
                }
            }
        }
        return mail::DecodeMailboxName(raw);
    }

    static std::string FolderRole(Editor &e, int account, const std::string &raw) {
        const mail::Account *a = AccountAt(e, account);
        if (!a) return "";
        if (!a->trash.empty() && a->trash == raw) return "trash";
        if (!a->archive.empty() && a->archive == raw) return "archive";
        auto it = e.mail_.folders.find(a->id);
        if (it == e.mail_.folders.end()) return "";
        for (const mail::Folder &f : it->second) {
            if (f.raw == raw) return f.role;
        }
        return "";
    }

    // --- Jobs ------------------------------------------------------------

    // Runs `work` on a background thread; `done` runs on the main thread
    // afterwards (from JobManager::PollAll) with the session, or nullptr
    // when the pane went away meanwhile.
    static void Spawn(Editor &e, int buffer_id, const std::string &name,
                      std::function<void(const std::atomic<bool> &)> work, std::function<void(MailSession *)> done) {
        auto job_id = std::make_shared<int>(0);
        JobManager::Callbacks cb;
        Editor *ed = &e;
        cb.on_exit = [ed, buffer_id, job_id, done](int) {
            MailSession *s = Session(*ed, buffer_id);
            if (s) s->jobs.erase(std::remove(s->jobs.begin(), s->jobs.end(), *job_id), s->jobs.end());
            done(s);
            mep::NoteActivity();
        };
        Job::Task task = [work](const Job::EmitLine &, const std::atomic<bool> &cancelled) {
            work(cancelled);
            return 0;
        };
        *job_id = JobManager::Instance().SpawnTask(std::move(task), std::move(cb), "mail: " + name);
        if (MailSession *s = Session(e, buffer_id)) s->jobs.push_back(*job_id);
    }

    // --- Rendering -------------------------------------------------------

    struct Out {
        std::vector<std::string> lines;
        std::vector<Decoration> decos;
        std::vector<MailRow> rows;
        void Add(const std::string &line, MailRow row = {}) {
            lines.push_back(line);
            rows.push_back(std::move(row));
        }
        void Span(int col_start, int col_end, const char *hl, bool bold = false) {
            Decoration d;
            d.row = static_cast<int>(lines.size()) - 1;
            d.col_start = col_start;
            d.col_end = col_end;
            d.hl_group = hl;
            d.bold = bold;
            decos.push_back(d);
        }
        // The whole row's text in `hl` (a span, not whole_line: that one
        // tints the row's background).
        void Line(const char *hl) {
            if (lines.empty() || lines.back().empty()) return;
            Span(0, static_cast<int>(lines.back().size()), hl);
        }
    };

    // The pane header carries the title (the buffer itself starts with the
    // first folder/message, so gg/G/counts mean what they say).
    static void SetTitle(MailSession &s, const std::string &title, const std::string &rest, const std::string &status) {
        s.short_title = mail::FitWidth(title, std::min<size_t>(mail::DisplayWidth(title), 32));
        s.title = title;
        if (!rest.empty()) s.title += "  -  " + rest;
        if (!status.empty()) s.title += "  -  " + status;
    }

    static void RenderFolders(Editor &e, MailSession &s, Out &o) {
        std::string status = s.status;
        if (s.loading > 0 && status.empty()) status = "loading...";
        if (s.picking) {
            SetTitle(s, "Move " + Plural(s.pick_uids.size(), "message") + " to...", "Enter picks a folder, q cancels",
                     status);
        } else {
            SetTitle(s, "Folders", "", status);
        }
        for (size_t ai = 0; ai < e.mail_.accounts.size(); ai++) {
            const mail::Account &a = e.mail_.accounts[ai];
            const int acct = static_cast<int>(ai);
            if (s.picking && acct != s.account) continue;
            std::string head = "  " + (a.name.empty() ? a.email : a.name + " <" + a.email + ">");
            o.Add(head, {MailRow::Kind::Account, acct, -1, ""});
            o.Span(2, static_cast<int>(head.size()), "MailSender", true);
            auto it = e.mail_.folders.find(a.id);
            if (it == e.mail_.folders.end()) {
                o.Add("      INBOX", {MailRow::Kind::Folder, acct, -1, ""});
                o.Add("      (listing folders...)");
                o.Line("MailDate");
            } else {
                const std::vector<mail::Folder> &fs = it->second;
                // INBOX first, then the server's order.
                std::vector<size_t> order;
                for (size_t i = 0; i < fs.size(); i++) {
                    if (fs[i].raw == "INBOX") order.insert(order.begin(), i);
                    else order.push_back(i);
                }
                for (size_t i : order) {
                    const mail::Folder &f = fs[i];
                    size_t depth = 0;
                    std::string leaf = f.name;
                    for (char c : f.name) depth += c == f.delim ? 1 : 0;
                    const size_t cut = f.name.rfind(f.delim);
                    if (cut != std::string::npos) leaf = f.name.substr(cut + 1);
                    std::string line = "      " + std::string(depth * 2, ' ') + leaf;
                    o.Add(line, {f.selectable ? MailRow::Kind::Folder : MailRow::Kind::None, acct, static_cast<int>(i), ""});
                    if (!f.selectable) o.Line("MailDate");
                }
            }
            o.Add("");
        }
        if (s.picking) return;
        // Unsent drafts kept on this machine.
        const std::string dir = DraftsDir();
        std::vector<std::pair<std::filesystem::file_time_type, std::string>> drafts;
        std::error_code ec;
        if (!dir.empty()) {
            for (const auto &ent : std::filesystem::directory_iterator(dir, ec)) {
                if (ent.path().extension() != ".eml") continue;
                drafts.emplace_back(ent.last_write_time(ec), ent.path().string());
            }
        }
        if (drafts.empty()) return;
        std::sort(drafts.begin(), drafts.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
        o.Add("  Drafts on this computer");
        o.Span(2, 26, "MailSender", true);
        for (const auto &d : drafts) {
            std::string text;
            ReadFileBytes(d.second, &text);
            std::vector<std::string> lines = TextLines(text);
            const mail::Draft dr = mail::ParseDraft(lines);
            std::string line = "      " + (dr.subject.empty() ? std::string("(no subject)") : dr.subject);
            if (!dr.to.empty()) line += "  ->  " + dr.to;
            o.Add(line, {MailRow::Kind::Draft, -1, -1, d.second});
        }
    }

    static void RenderList(Editor &e, MailSession &s, Out &o) {
        const mail::Account *a = AccountAt(e, s.account);
        const std::string folder = FolderDisplayName(e, s.account, s.list.mailbox);
        std::string title = s.list.search.empty() ? folder : "Search \"" + s.list.search + "\" in " + folder;
        size_t unread = 0;
        for (const mail::MessageSummary &m : s.list.msgs) unread += m.Seen() ? 0 : 1;
        std::string rest = a ? a->email : "";
        if (s.list.loaded) {
            const size_t total = s.list.search.empty() && s.list.exists > 0 ? s.list.exists : s.list.msgs.size();
            rest += "   " + Plural(total, s.list.search.empty() ? "message" : "match");
            if (unread > 0) rest += ", " + std::to_string(unread) + " unread";
        }
        std::string status = s.status;
        if (s.loading > 0 && status.empty()) status = "loading...";
        SetTitle(s, title, rest, status);
        if (!s.list.loaded && s.loading > 0) {
            o.Add("  Loading...");
            o.Line("MailDate");
            return;
        }
        if (s.list.loaded && s.list.msgs.empty()) {
            o.Add(s.list.search.empty() ? "  This folder is empty." : "  Nothing matched.");
            o.Line("MailDate");
        }
        const long long now = static_cast<long long>(std::time(nullptr));
        for (size_t i = 0; i < s.list.msgs.size(); i++) {
            const mail::MessageSummary &m = s.list.msgs[i];
            std::string flags = "    ";
            if (!m.Seen()) flags[0] = 'N';
            if (m.Flagged()) flags[1] = '*';
            if (m.Answered()) flags[2] = 'r';
            if (m.has_attachment) flags[3] = '@';
            // Sent folders are about who it went to.
            const bool sent_view = FolderRole(e, s.account, s.list.mailbox) == "sent";
            std::string who;
            if (sent_view && !m.to.empty()) who = "To: " + m.to[0].Short();
            else if (!m.from.empty()) who = m.from[0].Short();
            const std::string date = mail::FitWidth(mail::FormatListDate(m.date, now), 10);
            const std::string sender = mail::FitWidth(who, 24);
            std::string subject = m.subject.empty() ? "(no subject)" : m.subject;
            for (char &c : subject) {
                if (static_cast<unsigned char>(c) < 0x20) c = ' ';
            }
            const std::string line = " " + flags + "  " + date + "  " + sender + "  " + subject;
            o.Add(line, {MailRow::Kind::Message, s.account, static_cast<int>(i), ""});
            // Spans only over the ASCII lead-in (flags, date) and from the
            // sender onwards -- decoration columns are byte offsets.
            const int sender_col = 1 + 4 + 2 + static_cast<int>(date.size()) + 2;
            if (!m.Seen()) o.Span(1, 2, "MailNew", true);
            if (m.Flagged()) o.Span(2, 3, "MailFlagged", true);
            o.Span(7, 7 + static_cast<int>(date.size()), "MailDate");
            if (!m.Seen()) {
                o.Span(sender_col, static_cast<int>(line.size()), "MailUnread", true);
                o.Span(sender_col, sender_col + static_cast<int>(mail::DisplayWidth(sender)), "MailSender", true);
            } else {
                o.Span(sender_col, sender_col + static_cast<int>(mail::DisplayWidth(sender)), "MailSender");
            }
        }
        if (s.list.search.empty() && s.list.min_seq > 1) {
            o.Add("");
            o.Add("  ... " + Plural(s.list.min_seq - 1, "older message") + " -- Enter or + loads " +
                      std::to_string(std::min<uint32_t>(kPageSize, s.list.min_seq - 1)) + " more",
                  {MailRow::Kind::LoadMore, s.account, -1, ""});
            o.Line("MailDate");
        }
    }

    static void RenderMessage(Editor &e, MailSession &s, Out &o) {
        (void)e;
        if (!s.msg_loaded) {
            std::string subject;
            for (const mail::MessageSummary &m : s.list.msgs) {
                if (m.uid == s.msg_uid) subject = m.subject;
            }
            SetTitle(s, subject.empty() ? std::string("(no subject)") : subject, "",
                     s.status.empty() ? std::string("loading...") : s.status);
            o.Add(s.status.empty() ? "Loading message..." : s.status);
            o.Line("MailDate");
            return;
        }
        const mail::Message &m = s.msg;
        SetTitle(s, m.subject.empty() ? std::string("(no subject)") : m.subject, "", s.status);
        auto header = [&](const char *label, const std::string &value) {
            if (value.empty()) return;
            std::string v = value;
            for (char &c : v) {
                if (static_cast<unsigned char>(c) < 0x20) c = ' ';
            }
            const std::string line = std::string(label) + v;
            o.Add(line);
            o.Span(0, static_cast<int>(std::strlen(label)), "MailHeader", true);
        };
        auto list = [](const std::vector<mail::Address> &as) {
            std::string out;
            for (const mail::Address &a : as) out += (out.empty() ? "" : ", ") + a.Display();
            return out;
        };
        const std::string subject = m.subject.empty() ? "(no subject)" : m.subject;
        o.Add("Subject: " + subject);
        o.Span(0, 8, "MailHeader", true);
        o.Span(9, 9 + static_cast<int>(mail::DisplayWidth(subject)), "MailTitle", true);
        header("From:    ", list(m.from));
        if (!m.reply_to.empty() && list(m.reply_to) != list(m.from)) header("Reply-To:", " " + list(m.reply_to));
        header("To:      ", list(m.to));
        header("Cc:      ", list(m.cc));
        header("Date:    ", m.date >= 0 ? mail::FormatLongDate(m.date) : m.date_raw);
        for (size_t i = 0; i < m.attachments.size(); i++) {
            const mail::Attachment &att = m.attachments[i];
            const std::string line = "Attach:  [" + std::to_string(i + 1) + "] " + att.filename + "  (" + att.content_type +
                                     ", " + mail::HumanSize(att.data.size()) + ")";
            o.Add(line, {MailRow::Kind::Attachment, s.account, static_cast<int>(i), ""});
            o.Span(0, 7, "MailHeader", true);
            o.Span(9, static_cast<int>(line.size()), "MailNew");
        }
        if (m.text_from_html) {
            o.Add("         (shown as text from the message's HTML)");
            o.Line("MailDate");
        }
        o.Add(std::string(40, '-'));
        o.Line("MailDate");
        o.Add("");
        bool in_sig = false;
        for (const std::string &line : TextLines(m.text)) {
            if (line == "-- ") in_sig = true;
            o.Add(line);
            if (!line.empty() && line[0] == '>') o.Line("MailQuote");
            else if (in_sig) o.Line("MailRead");
        }
    }

    static void Render(Editor &e, MailSession &s) {
        if (s.buffer_id < 0 || s.buffer_id >= static_cast<int>(e.buffers_.size())) return;
        Out o;
        switch (s.view) {
            case MailSession::View::Folders: RenderFolders(e, s, o); break;
            case MailSession::View::List: RenderList(e, s, o); break;
            case MailSession::View::Message: RenderMessage(e, s, o); break;
        }
        Buffer &b = e.buffers_[static_cast<size_t>(s.buffer_id)];
        b.lines = o.lines.empty() ? std::vector<std::string>{""} : o.lines;
        b.modified = false;
        b.undo_stack.clear();
        b.redo_stack.clear();
        b.folds.clear();
        b.hide_line_numbers = true;
        const bool message = s.view == MailSession::View::Message;
        b.row_cursor = !message;
        b.no_wrap = !message;
        switch (s.view) {
            case MailSession::View::Folders:
                b.footer_hint = s.picking ? "Enter move here  q cancel"
                                          : "Enter open  c compose  $ reload  q close  ? help";
                break;
            case MailSession::View::List:
                b.footer_hint = "Enter read  c new  r/R/f reply/all/fwd  dd delete  a archive  M move  s star  "
                                "U unread  $ refresh  S search  - folders  ? help";
                break;
            case MailSession::View::Message:
                b.footer_hint = "q back  J/K next/prev  r/R/f reply/all/fwd  dd delete  a archive  M move  s star  "
                                "U unread  ? help";
                break;
        }
        b.footer_hint_hl = "Comment";
        s.rows = std::move(o.rows);
        e.ClearNamespaceInBuffer(s.buffer_id, e.mail_.ns);
        for (Decoration &d : o.decos) e.AddDecorationToBuffer(s.buffer_id, e.mail_.ns, d);
        e.change_epoch_++;
        if (Pane *p = PaneShowing(e, s.buffer_id)) {
            p->cursor.row = std::clamp(p->cursor.row, 0, static_cast<int>(b.lines.size()) - 1);
            if (!message) p->cursor.col = 0;
        }
    }

    // --- Loading ---------------------------------------------------------

    static void SortNewestFirst(std::vector<mail::MessageSummary> *msgs) {
        std::sort(msgs->begin(), msgs->end(),
                  [](const mail::MessageSummary &a, const mail::MessageSummary &b) { return a.uid > b.uid; });
    }

    static void CacheFolders(Editor &e, const std::string &account_id, std::vector<mail::Folder> folders) {
        if (!folders.empty()) e.mail_.folders[account_id] = std::move(folders);
    }

    static void LoadFolders(Editor &e, MailSession &s, int account) {
        auto cred = MakeCred(e, account);
        auto res = std::make_shared<std::pair<std::vector<mail::Folder>, std::string>>();
        s.loading++;
        Editor *ed = &e;
        Spawn(
            e, s.buffer_id, "folders",
            [cred, res](const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    res->second = cred->error;
                    return;
                }
                if (!mail::ListFolders(cred->Session(), &res->first, &res->second, &cancel) && res->second.empty()) {
                    res->second = "could not list folders";
                }
            },
            [ed, cred, res](MailSession *sp) {
                AfterCred(*ed, *cred, res->second);
                if (!res->second.empty()) {
                    if (res->second != "cancelled") {
                        ed->Notify("Mail (" + cred->account.email + "): " + res->second, Editor::NotifyLevel::Error);
                    }
                } else {
                    CacheFolders(*ed, cred->account.id, res->first);
                }
                if (!sp) return;
                sp->loading = std::max(0, sp->loading - 1);
                if (sp->view == MailSession::View::Folders) Render(*ed, *sp);
            });
    }

    static void ShowFolders(Editor &e, MailSession &s) {
        if (s.view == MailSession::View::List) s.list_row = CursorRow(e, s);
        s.view = MailSession::View::Folders;
        s.status.clear();
        s.pending_d = false;
        for (size_t i = 0; i < e.mail_.accounts.size(); i++) {
            if (e.mail_.folders.find(e.mail_.accounts[i].id) == e.mail_.folders.end()) LoadFolders(e, s, static_cast<int>(i));
        }
        Render(e, s);
        SetCursorRow(e, s, s.folders_row);
    }

    struct ListResult {
        std::string error;
        mail::SelectInfo sel;
        std::vector<mail::MessageSummary> msgs;
        std::vector<mail::Folder> folders;
    };

    static void OpenMailbox(Editor &e, MailSession &s, int account, const std::string &mailbox) {
        if (s.view == MailSession::View::Folders) s.folders_row = CursorRow(e, s);
        s.view = MailSession::View::List;
        s.account = account;
        s.picking = false;
        s.pending_d = false;
        s.list = MailListState{};
        s.list.mailbox = mailbox;
        s.generation++;
        s.status.clear();
        s.loading++;
        s.list_row = kFirstRow;
        Render(e, s);
        SetCursorRow(e, s, kFirstRow);
        const int gen = s.generation;
        auto cred = MakeCred(e, account);
        auto res = std::make_shared<ListResult>();
        const bool need_folders = cred && e.mail_.folders.find(cred->account.id) == e.mail_.folders.end();
        Editor *ed = &e;
        Spawn(
            e, s.buffer_id, "open " + mailbox,
            [cred, res, mailbox, need_folders](const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    res->error = cred->error;
                    return;
                }
                const mail::Session sess = cred->Session();
                if (need_folders) {
                    std::string ignored;
                    mail::ListFolders(sess, &res->folders, &ignored, &cancel);
                }
                if (!mail::SelectMailbox(sess, mailbox, &res->sel, &res->error, &cancel)) return;
                if (res->sel.exists == 0) return;
                const uint32_t from = res->sel.exists > kPageSize ? res->sel.exists - kPageSize + 1 : 1;
                mail::FetchSummaries(sess, mailbox, std::to_string(from) + ":*", false, &res->msgs, &res->error, &cancel);
            },
            [ed, cred, res, gen](MailSession *sp) {
                AfterCred(*ed, *cred, res->error);
                CacheFolders(*ed, cred->account.id, res->folders);
                if (!sp) return;
                sp->loading = std::max(0, sp->loading - 1);
                if (sp->generation != gen) return;
                if (!res->error.empty()) {
                    sp->status = res->error;
                    ReportError(*ed, res->error);
                    sp->list.loaded = true;
                    Render(*ed, *sp);
                    return;
                }
                sp->list.msgs = std::move(res->msgs);
                SortNewestFirst(&sp->list.msgs);
                sp->list.exists = res->sel.exists;
                sp->list.min_seq = 0;
                for (const mail::MessageSummary &m : sp->list.msgs) {
                    if (sp->list.min_seq == 0 || (m.seq > 0 && m.seq < sp->list.min_seq)) sp->list.min_seq = m.seq;
                }
                sp->list.loaded = true;
                sp->last_refresh = gfx::GetTime();
                Render(*ed, *sp);
            });
    }

    static void Refresh(Editor &e, MailSession &s, bool quiet) {
        if (s.view == MailSession::View::Folders) {
            for (const mail::Account &a : e.mail_.accounts) e.mail_.folders.erase(a.id);
            ShowFolders(e, s);
            return;
        }
        if (!s.list.search.empty()) {
            Search(e, s, s.list.search);
            return;
        }
        if (!s.list.loaded || s.list.msgs.empty()) {
            if (s.view == MailSession::View::List) OpenMailbox(e, s, s.account, s.list.mailbox);
            return;
        }
        uint32_t min_uid = s.list.msgs.back().uid, max_uid = 0;
        for (const mail::MessageSummary &m : s.list.msgs) {
            min_uid = std::min(min_uid, m.uid);
            max_uid = std::max(max_uid, m.uid);
        }
        s.last_refresh = gfx::GetTime();
        s.loading++;
        if (!quiet) Render(e, s);
        const int gen = s.generation;
        auto cred = MakeCred(e, s.account);
        auto res = std::make_shared<ListResult>();
        const std::string mailbox = s.list.mailbox;
        Editor *ed = &e;
        Spawn(
            e, s.buffer_id, "refresh " + mailbox,
            [cred, res, mailbox, min_uid](const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    res->error = cred->error;
                    return;
                }
                mail::FetchSummaries(cred->Session(), mailbox, std::to_string(min_uid) + ":*", true, &res->msgs,
                                     &res->error, &cancel);
            },
            [ed, cred, res, gen, min_uid, max_uid, quiet](MailSession *sp) {
                AfterCred(*ed, *cred, res->error);
                if (!sp) return;
                sp->loading = std::max(0, sp->loading - 1);
                if (sp->generation != gen) return;
                if (!res->error.empty()) {
                    if (!quiet) ReportError(*ed, res->error);
                    sp->status = quiet ? std::string() : res->error;
                    Render(*ed, *sp);
                    return;
                }
                // Keep the cursor on the same message across the update.
                uint32_t cursor_uid = 0;
                const int row = CursorRow(*ed, *sp);
                if (sp->view == MailSession::View::List && row >= 0 && row < static_cast<int>(sp->rows.size()) &&
                    sp->rows[static_cast<size_t>(row)].kind == MailRow::Kind::Message) {
                    cursor_uid = sp->list.msgs[static_cast<size_t>(sp->rows[static_cast<size_t>(row)].index)].uid;
                }
                std::vector<mail::MessageSummary> fresh;
                size_t arrived = 0;
                uint32_t max_seq = 0;
                for (mail::MessageSummary &m : res->msgs) {
                    if (m.uid < min_uid) continue;
                    if (m.uid > max_uid && !m.Seen()) arrived++;
                    max_seq = std::max(max_seq, m.seq);
                    fresh.push_back(std::move(m));
                }
                SortNewestFirst(&fresh);
                sp->list.msgs = std::move(fresh);
                if (max_seq > 0) sp->list.exists = max_seq;
                sp->list.min_seq = 0;
                for (const mail::MessageSummary &m : sp->list.msgs) {
                    if (sp->list.min_seq == 0 || (m.seq > 0 && m.seq < sp->list.min_seq)) sp->list.min_seq = m.seq;
                }
                sp->status.clear();
                Render(*ed, *sp);
                if (sp->view == MailSession::View::List && cursor_uid != 0) {
                    for (size_t i = 0; i < sp->list.msgs.size(); i++) {
                        if (sp->list.msgs[i].uid == cursor_uid) SetCursorRow(*ed, *sp, kFirstRow + static_cast<int>(i));
                    }
                }
                if (arrived > 0) {
                    ed->Notify("Mail: " + Plural(arrived, "new message") + " in " +
                                   FolderDisplayName(*ed, sp->account, sp->list.mailbox),
                               Editor::NotifyLevel::Info);
                }
            });
    }

    static void LoadMore(Editor &e, MailSession &s) {
        if (!s.list.search.empty() || s.list.min_seq <= 1) {
            e.Notify("Mail: no older messages", Editor::NotifyLevel::Info);
            return;
        }
        const uint32_t to = s.list.min_seq - 1;
        const uint32_t from = to > kPageSize ? to - kPageSize + 1 : 1;
        s.loading++;
        Render(e, s);
        const int gen = s.generation;
        auto cred = MakeCred(e, s.account);
        auto res = std::make_shared<ListResult>();
        const std::string mailbox = s.list.mailbox;
        Editor *ed = &e;
        Spawn(
            e, s.buffer_id, "older " + mailbox,
            [cred, res, mailbox, from, to](const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    res->error = cred->error;
                    return;
                }
                mail::FetchSummaries(cred->Session(), mailbox, std::to_string(from) + ":" + std::to_string(to), false,
                                     &res->msgs, &res->error, &cancel);
            },
            [ed, cred, res, gen, from](MailSession *sp) {
                AfterCred(*ed, *cred, res->error);
                if (!sp) return;
                sp->loading = std::max(0, sp->loading - 1);
                if (sp->generation != gen) return;
                if (!res->error.empty()) {
                    ReportError(*ed, res->error);
                    Render(*ed, *sp);
                    return;
                }
                for (mail::MessageSummary &m : res->msgs) {
                    bool dup = false;
                    for (const mail::MessageSummary &have : sp->list.msgs) dup = dup || have.uid == m.uid;
                    if (!dup) sp->list.msgs.push_back(std::move(m));
                }
                SortNewestFirst(&sp->list.msgs);
                sp->list.min_seq = from;
                Render(*ed, *sp);
            });
    }

    static void Search(Editor &e, MailSession &s, const std::string &query) {
        const std::string mailbox = s.list.mailbox.empty() ? std::string("INBOX") : s.list.mailbox;
        s.view = MailSession::View::List;
        s.list = MailListState{};
        s.list.mailbox = mailbox;
        s.list.search = query;
        s.generation++;
        s.loading++;
        s.status.clear();
        Render(e, s);
        SetCursorRow(e, s, kFirstRow);
        const int gen = s.generation;
        auto cred = MakeCred(e, s.account);
        auto res = std::make_shared<ListResult>();
        Editor *ed = &e;
        Spawn(
            e, s.buffer_id, "search",
            [cred, res, mailbox, query](const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    res->error = cred->error;
                    return;
                }
                std::vector<uint32_t> uids;
                if (!mail::SearchMailbox(cred->Session(), mailbox, query, &uids, &res->error, &cancel)) return;
                std::sort(uids.begin(), uids.end());
                if (uids.size() > kSearchLimit) uids.erase(uids.begin(), uids.end() - static_cast<long>(kSearchLimit));
                if (uids.empty()) return;
                mail::FetchSummaries(cred->Session(), mailbox, mail::ImapSet(uids), true, &res->msgs, &res->error, &cancel);
            },
            [ed, cred, res, gen](MailSession *sp) {
                AfterCred(*ed, *cred, res->error);
                if (!sp) return;
                sp->loading = std::max(0, sp->loading - 1);
                if (sp->generation != gen) return;
                if (!res->error.empty()) {
                    sp->status = res->error;
                    ReportError(*ed, res->error);
                }
                sp->list.msgs = std::move(res->msgs);
                SortNewestFirst(&sp->list.msgs);
                sp->list.loaded = true;
                Render(*ed, *sp);
            });
    }

    // --- Messages --------------------------------------------------------

    static int IndexOfUid(const MailSession &s, uint32_t uid) {
        for (size_t i = 0; i < s.list.msgs.size(); i++) {
            if (s.list.msgs[i].uid == uid) return static_cast<int>(i);
        }
        return -1;
    }

    struct MessageResult {
        std::string error;
        mail::Message msg;
    };

    static void OpenMessage(Editor &e, MailSession &s, int index) {
        if (index < 0 || index >= static_cast<int>(s.list.msgs.size())) return;
        if (s.view == MailSession::View::List) s.list_row = CursorRow(e, s);
        else s.list_row = kFirstRow + index;
        s.view = MailSession::View::Message;
        s.pending_d = false;
        s.msg_uid = s.list.msgs[static_cast<size_t>(index)].uid;
        s.msg = mail::Message{};
        s.msg_loaded = false;
        s.status.clear();
        s.generation++;
        s.loading++;
        Render(e, s);
        SetCursorRow(e, s, 0);
        if (Pane *p = PaneShowing(e, s.buffer_id)) p->scroll_row = 0;
        const int gen = s.generation;
        const uint32_t uid = s.msg_uid;
        auto cred = MakeCred(e, s.account);
        auto res = std::make_shared<MessageResult>();
        const std::string mailbox = s.list.mailbox;
        Editor *ed = &e;
        Spawn(
            e, s.buffer_id, "message",
            [cred, res, mailbox, uid](const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    res->error = cred->error;
                    return;
                }
                std::string raw;
                if (!mail::FetchMessage(cred->Session(), mailbox, uid, &raw, &res->error, &cancel)) return;
                if (raw.empty()) {
                    res->error = "the message is gone (moved or deleted elsewhere?)";
                    return;
                }
                res->msg = mail::ParseMessage(raw);
            },
            [ed, cred, res, gen, uid](MailSession *sp) {
                AfterCred(*ed, *cred, res->error);
                if (!sp) return;
                sp->loading = std::max(0, sp->loading - 1);
                if (sp->generation != gen) return;
                if (!res->error.empty()) {
                    sp->status = res->error;
                    ReportError(*ed, res->error);
                    Render(*ed, *sp);
                    return;
                }
                sp->msg = std::move(res->msg);
                sp->msg_loaded = true;
                const int idx = IndexOfUid(*sp, uid);
                if (idx >= 0) sp->list.msgs[static_cast<size_t>(idx)].SetFlag("\\Seen", true);  // reading marks it read
                Render(*ed, *sp);
                SetCursorRow(*ed, *sp, 0);
            });
    }

    static void BackToList(Editor &e, MailSession &s) {
        s.view = MailSession::View::List;
        s.pending_d = false;
        s.status.clear();
        // An in-flight message load is now moot.
        s.generation++;
        s.loading = 0;
        if (!s.list.loaded) {
            OpenMailbox(e, s, s.account, s.list.mailbox);
            return;
        }
        Render(e, s);
        SetCursorRow(e, s, s.list_row);
    }

    static void StepMessage(Editor &e, MailSession &s, int delta) {
        const int idx = IndexOfUid(s, s.msg_uid);
        const int next = idx + delta;
        if (idx < 0 || next < 0 || next >= static_cast<int>(s.list.msgs.size())) {
            e.Notify(delta > 0 ? "Mail: no older message loaded" : "Mail: this is the newest message", Editor::NotifyLevel::Info);
            return;
        }
        OpenMessage(e, s, next);
    }

    // The messages a command acts on: in the list, `count` rows from the
    // cursor; in a message, that message.
    static std::vector<int> Targets(Editor &e, MailSession &s, int count) {
        std::vector<int> out;
        if (s.view == MailSession::View::Message) {
            const int idx = IndexOfUid(s, s.msg_uid);
            if (idx >= 0) out.push_back(idx);
            return out;
        }
        if (s.view != MailSession::View::List) return out;
        const int row = CursorRow(e, s);
        for (int r = row; r < row + std::max(1, count) && r < static_cast<int>(s.rows.size()); r++) {
            const MailRow &mr = s.rows[static_cast<size_t>(r)];
            if (mr.kind != MailRow::Kind::Message) break;
            out.push_back(mr.index);
        }
        return out;
    }

    static std::vector<uint32_t> UidsOf(const MailSession &s, const std::vector<int> &idxs) {
        std::vector<uint32_t> uids;
        for (int i : idxs) uids.push_back(s.list.msgs[static_cast<size_t>(i)].uid);
        return uids;
    }

    // Removes messages from the list right away (the server is told in the
    // background) and, in the message view, moves on to the next one.
    static std::vector<mail::MessageSummary> RemoveLocally(Editor &e, MailSession &s, const std::vector<int> &idxs) {
        std::vector<mail::MessageSummary> removed;
        std::vector<int> sorted = idxs;
        std::sort(sorted.rbegin(), sorted.rend());
        const int first = sorted.empty() ? 0 : sorted.back();
        for (int i : sorted) {
            removed.push_back(s.list.msgs[static_cast<size_t>(i)]);
            s.list.msgs.erase(s.list.msgs.begin() + i);
        }
        if (s.list.exists >= removed.size()) s.list.exists -= static_cast<uint32_t>(removed.size());
        if (s.view == MailSession::View::Message) {
            if (first < static_cast<int>(s.list.msgs.size())) {
                OpenMessage(e, s, first);
            } else {
                BackToList(e, s);
            }
        } else {
            const int row = CursorRow(e, s);
            Render(e, s);
            SetCursorRow(e, s, row);
        }
        return removed;
    }

    static void RestoreRemoved(Editor &e, MailSession &s, std::vector<mail::MessageSummary> removed) {
        for (mail::MessageSummary &m : removed) {
            if (IndexOfUid(s, m.uid) < 0) s.list.msgs.push_back(std::move(m));
        }
        SortNewestFirst(&s.list.msgs);
        if (s.view != MailSession::View::Message) Render(e, s);
    }

    struct MoveResult {
        std::string error;
        std::string dest;
        std::vector<mail::Folder> folders;
    };

    // Moves the messages at `idxs` to `dest`, or to the folder playing
    // `role` ("trash", "archive") when `dest` is empty. Deleting from the
    // Trash itself removes for good.
    static void MoveMessages(Editor &e, MailSession &s, const std::vector<int> &idxs, const std::string &role,
                             const std::string &dest_in) {
        if (idxs.empty()) return;
        const mail::Account *acct = AccountAt(e, s.account);
        if (!acct) return;
        const std::vector<uint32_t> uids = UidsOf(s, idxs);
        const std::string mailbox = s.list.mailbox;
        std::vector<mail::Folder> folders;
        auto fit = e.mail_.folders.find(acct->id);
        if (fit != e.mail_.folders.end()) folders = fit->second;
        if (role == "archive" && FolderRole(e, s.account, mailbox) == "archive") {
            e.Notify("Mail: already in the archive", Editor::NotifyLevel::Info);
            return;
        }
        std::vector<mail::MessageSummary> removed = RemoveLocally(e, s, idxs);
        auto cred = MakeCred(e, s.account);
        auto res = std::make_shared<MoveResult>();
        const size_t n = uids.size();
        Editor *ed = &e;
        Spawn(
            e, s.buffer_id, role.empty() ? "move" : role,
            [cred, res, uids, mailbox, role, dest_in, folders](const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    res->error = cred->error;
                    return;
                }
                const mail::Session sess = cred->Session();
                std::string dest = dest_in;
                if (dest.empty()) {
                    dest = FindRoleFolder(cred->account, folders, role);
                    if (dest.empty() && folders.empty()) {
                        std::string ignored;
                        mail::ListFolders(sess, &res->folders, &ignored, &cancel);
                        dest = FindRoleFolder(cred->account, res->folders, role);
                    }
                    if (dest.empty()) {
                        res->error = "no " + role + " folder found -- set \"" + role + "\" for this account in " +
                                     mail::AccountsPath();
                        return;
                    }
                }
                res->dest = dest;
                if (dest == mailbox) {
                    if (role != "trash") {
                        res->error = "already in that folder";
                        return;
                    }
                    // Deleting from the Trash: gone for good.
                    const std::string set = mail::ImapSet(uids);
                    mail::ImapResult r = mail::RunImap(sess,
                                                       {{mailbox, "UID STORE " + set + " +FLAGS.SILENT (\\Deleted)", ""},
                                                        {mailbox, "UID EXPUNGE " + set, ""}},
                                                       true, &cancel);
                    if (!r.ok) {
                        mail::ImapResult r2 = mail::RunImap(sess, {{mailbox, "EXPUNGE", ""}}, true, &cancel);
                        if (!r2.ok) res->error = r.error;
                    }
                    return;
                }
                mail::MoveMessages(sess, mailbox, uids, dest, &res->error, &cancel);
            },
            [ed, cred, res, removed, role, n](MailSession *sp) mutable {
                AfterCred(*ed, *cred, res->error);
                CacheFolders(*ed, cred->account.id, res->folders);
                if (!res->error.empty()) {
                    ReportError(*ed, res->error);
                    if (sp) RestoreRemoved(*ed, *sp, std::move(removed));
                    return;
                }
                std::string what = role == "trash" ? "Deleted" : role == "archive" ? "Archived" : "Moved";
                std::string where;
                if (role.empty() && sp) where = " to " + FolderDisplayName(*ed, sp->account, res->dest);
                ed->SetStatusMessage("Mail: " + what + " " + Plural(n, "message") + where);
            });
    }

    static void StartMovePick(Editor &e, MailSession &s, int count) {
        const std::vector<int> idxs = Targets(e, s, count);
        if (idxs.empty()) return;
        s.pick_uids = UidsOf(s, idxs);
        if (s.view == MailSession::View::Message) s.list_row = kFirstRow + idxs[0];
        else s.list_row = CursorRow(e, s);
        s.picking = true;
        s.view = MailSession::View::Folders;
        const mail::Account *a = AccountAt(e, s.account);
        if (a && e.mail_.folders.find(a->id) == e.mail_.folders.end()) LoadFolders(e, s, s.account);
        Render(e, s);
        SetCursorRow(e, s, kFirstRow + 1);  // the account's INBOX row
    }

    static void FinishMovePick(Editor &e, MailSession &s, const std::string &dest) {
        std::vector<uint32_t> uids = s.pick_uids;
        s.picking = false;
        s.pick_uids.clear();
        s.view = MailSession::View::List;
        Render(e, s);
        SetCursorRow(e, s, s.list_row);
        std::vector<int> idxs;
        for (uint32_t uid : uids) {
            const int i = IndexOfUid(s, uid);
            if (i >= 0) idxs.push_back(i);
        }
        if (dest == s.list.mailbox) {
            e.Notify("Mail: already in that folder", Editor::NotifyLevel::Info);
            return;
        }
        MoveMessages(e, s, idxs, "", dest);
    }

    static void ToggleFlag(Editor &e, MailSession &s, int count, const std::string &flag) {
        const std::vector<int> idxs = Targets(e, s, count);
        if (idxs.empty()) return;
        bool any_off = false;
        for (int i : idxs) any_off = any_off || !s.list.msgs[static_cast<size_t>(i)].HasFlag(flag);
        const bool on = any_off;
        std::vector<std::pair<uint32_t, bool>> before;
        for (int i : idxs) {
            mail::MessageSummary &m = s.list.msgs[static_cast<size_t>(i)];
            before.emplace_back(m.uid, m.HasFlag(flag));
            m.SetFlag(flag, on);
        }
        if (s.view == MailSession::View::List) {
            const int row = CursorRow(e, s);
            Render(e, s);
            // Like dd, a counted command lands past what it touched.
            SetCursorRow(e, s, count > 1 ? row + static_cast<int>(idxs.size()) : row);
        }
        auto cred = MakeCred(e, s.account);
        auto err = std::make_shared<std::string>();
        const std::vector<uint32_t> uids = UidsOf(s, idxs);
        const std::string mailbox = s.list.mailbox;
        Editor *ed = &e;
        Spawn(
            e, s.buffer_id, "flag",
            [cred, err, uids, mailbox, flag, on](const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    *err = cred->error;
                    return;
                }
                mail::StoreFlags(cred->Session(), mailbox, uids, on ? "+FLAGS" : "-FLAGS", "(" + flag + ")", err.get(), &cancel);
            },
            [ed, cred, err, before, flag](MailSession *sp) {
                AfterCred(*ed, *cred, *err);
                if (err->empty()) return;
                ReportError(*ed, *err);
                if (!sp) return;
                for (const auto &b : before) {
                    const int i = IndexOfUid(*sp, b.first);
                    if (i >= 0) sp->list.msgs[static_cast<size_t>(i)].SetFlag(flag, b.second);
                }
                if (sp->view == MailSession::View::List) Render(*ed, *sp);
            });
        if (flag == "\\Seen") e.SetStatusMessage(on ? "Mail: marked read" : "Mail: marked unread");
        else e.SetStatusMessage(on ? "Mail: starred" : "Mail: unstarred");
    }

    // Runs `then` with the full message at list index `idx`: the open one
    // when it is that message, otherwise fetched first.
    static void WithMessage(Editor &e, MailSession &s, int idx,
                            std::function<void(MailSession &, const mail::Message &, uint32_t)> then) {
        if (idx < 0 || idx >= static_cast<int>(s.list.msgs.size())) return;
        const uint32_t uid = s.list.msgs[static_cast<size_t>(idx)].uid;
        if (s.view == MailSession::View::Message && s.msg_loaded && s.msg_uid == uid) {
            then(s, s.msg, uid);
            return;
        }
        e.SetStatusMessage("Mail: fetching the message...");
        auto cred = MakeCred(e, s.account);
        auto res = std::make_shared<MessageResult>();
        const std::string mailbox = s.list.mailbox;
        Editor *ed = &e;
        Spawn(
            e, s.buffer_id, "fetch",
            [cred, res, mailbox, uid](const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    res->error = cred->error;
                    return;
                }
                std::string raw;
                if (mail::FetchMessage(cred->Session(), mailbox, uid, &raw, &res->error, &cancel)) {
                    res->msg = mail::ParseMessage(raw);
                }
            },
            [ed, cred, res, uid, then](MailSession *sp) {
                AfterCred(*ed, *cred, res->error);
                if (!res->error.empty()) {
                    ReportError(*ed, res->error);
                    return;
                }
                if (!sp) return;
                const int i = IndexOfUid(*sp, uid);
                if (i >= 0) sp->list.msgs[static_cast<size_t>(i)].SetFlag("\\Seen", true);
                ed->SetStatusMessage("");
                then(*sp, res->msg, uid);
            });
    }

    // --- Compose ---------------------------------------------------------

    static void Compose(Editor &e, int account, mail::Draft d, MailDraftContext ctx, bool cursor_in_body) {
        const mail::Account *a = AccountAt(e, account);
        const std::string dir = DraftsDir();
        if (!a || dir.empty()) {
            e.Notify("Mail: nowhere to keep the draft", Editor::NotifyLevel::Error);
            return;
        }
        if (d.from.empty()) d.from = a->FromLine();
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        const std::string path = dir + "/" + stamp + "-" + RandomSuffix() + ".eml";
        const std::vector<std::string> lines = mail::DraftLines(d);
        if (!WriteFileBytes(path, JoinDraftLines(lines))) {
            e.Notify("Mail: cannot write " + path, Editor::NotifyLevel::Error);
            return;
        }
        ctx.account_id = a->id;
        if (IsMail(e, e.CurPane().buffer_id)) ctx.return_buffer = e.CurPane().buffer_id;
        e.mail_.drafts[AbsKey(path)] = ctx;
        e.LoadFile(path);
        DecorateDraftBuffer(e);
        // The cursor: on the To: line for a new message, on the body's first
        // line for a reply (the header block ends at the first empty line).
        int row = 1;
        if (cursor_in_body) {
            for (size_t i = 0; i < e.Buf().lines.size(); i++) {
                if (e.Buf().lines[i].empty()) {
                    row = static_cast<int>(i) + 1;
                    break;
                }
            }
        }
        e.CurPane().cursor.row = std::min(row, static_cast<int>(e.Buf().lines.size()) - 1);
        e.CurPane().cursor.col = cursor_in_body ? 0 : static_cast<int>(e.Buf().lines[static_cast<size_t>(e.CurPane().cursor.row)].size());
        e.ClampCursor();
    }

    static void DecorateDraftBuffer(Editor &e) {
        e.Buf().footer_hint = ":MailSend send   :MailAttach <file> attach   :w save draft   :MailDiscard discard";
        e.Buf().footer_hint_hl = "Comment";
    }

    static std::string Signature(const mail::Account &a) { return a.signature.empty() ? "" : "\n-- \n" + a.signature + "\n"; }

    static void Reply(Editor &e, MailSession &s, bool all) {
        const std::vector<int> idxs = Targets(e, s, 1);
        if (idxs.empty()) return;
        WithMessage(e, s, idxs[0], [&e, all](MailSession &sp, const mail::Message &m, uint32_t uid) {
            const mail::Account *a = AccountAt(e, sp.account);
            if (!a) return;
            mail::Draft d = mail::MakeReplyDraft(m, a->FromLine(), a->email, all);
            // The signature sits between the reply and the quoted original.
            if (!a->signature.empty()) d.body = "\n" + Signature(*a) + d.body.substr(1);
            MailDraftContext ctx;
            ctx.mailbox = sp.list.mailbox;
            ctx.reply_uid = uid;
            Compose(e, sp.account, d, ctx, true);
        });
    }

    static void Forward(Editor &e, MailSession &s) {
        const std::vector<int> idxs = Targets(e, s, 1);
        if (idxs.empty()) return;
        WithMessage(e, s, idxs[0], [&e](MailSession &sp, const mail::Message &m, uint32_t) {
            const mail::Account *a = AccountAt(e, sp.account);
            if (!a) return;
            mail::Draft d = mail::MakeForwardDraft(m, a->FromLine());
            if (!a->signature.empty()) d.body = Signature(*a) + d.body;
            MailDraftContext ctx;
            // The original's attachments ride along: written out so the
            // draft can name them like any other file.
            if (!m.attachments.empty()) {
                const std::string dir = mail::MailDataDir() + "/tmp/fwd-" + RandomSuffix();
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                for (const mail::Attachment &att : m.attachments) {
                    const std::string path = UniquePath(dir, mail::SafeFilename(att.filename));
                    if (WriteFileBytes(path, att.data)) {
                        d.attach.push_back(path);
                        ctx.temp_files.push_back(path);
                    }
                }
            }
            Compose(e, sp.account, d, ctx, false);
        });
    }

    static bool IsMail(Editor &e, int buffer_id) { return e.mail_.sessions.find(buffer_id) != e.mail_.sessions.end(); }

    static std::string DraftKey(Editor &e, int buffer_id) {
        if (buffer_id < 0 || buffer_id >= static_cast<int>(e.buffers_.size())) return "";
        const Buffer &b = e.buffers_[static_cast<size_t>(buffer_id)];
        if (b.filename.empty()) return "";
        return AbsKey(e.BufferAbsolutePath(b));
    }

    static const MailDraftContext *DraftContext(Editor &e, const std::string &key) {
        auto it = e.mail_.drafts.find(key);
        return it == e.mail_.drafts.end() ? nullptr : &it->second;
    }

    // Leaves a finished/discarded draft: its pane goes back to the mail
    // pane it came from, and the draft's buffer and file are removed.
    static void CloseDraft(Editor &e, int draft_buffer, const std::string &key) {
        MailDraftContext ctx;
        if (const MailDraftContext *c = DraftContext(e, key)) ctx = *c;
        int back = ctx.return_buffer;
        if (!IsMail(e, back)) back = WorkspaceMailBuffer(e);
        if (e.CurPane().buffer_id == draft_buffer && back >= 0) {
            ShowBufferHere(e, back);
            if (MailSession *s = Session(e, back)) {
                Render(e, *s);
                SetCursorRow(e, *s, s->view == MailSession::View::List ? s->list_row : 0);
            }
        }
        if (draft_buffer >= 0 && draft_buffer < static_cast<int>(e.buffers_.size())) {
            e.buffers_[static_cast<size_t>(draft_buffer)].modified = false;
            e.BufferDeleteById(draft_buffer, true);
        }
        std::remove(key.c_str());
        for (const std::string &t : ctx.temp_files) std::remove(t.c_str());
        e.mail_.drafts.erase(key);
        e.SyncModeToActivePaneBuffer();
    }

    // --- Attachments -----------------------------------------------------

    static void SaveAttachment(Editor &e, MailSession &s, int index) {
        if (!s.msg_loaded || index < 0 || index >= static_cast<int>(s.msg.attachments.size())) return;
        const mail::Attachment &att = s.msg.attachments[static_cast<size_t>(index)];
        const std::string path = UniquePath(DownloadsDir(), mail::SafeFilename(att.filename));
        if (!WriteFileBytes(path, att.data)) {
            e.Notify("Mail: cannot write " + path, Editor::NotifyLevel::Error);
            return;
        }
        if (ViewableInMep(path)) {
            e.SplitCurrentPane(SplitDir::Vertical, path);
            e.Notify("Saved " + path, Editor::NotifyLevel::Info);
        } else {
            e.Notify("Saved " + path + " (mep has no viewer for this type)", Editor::NotifyLevel::Info);
        }
    }

    // --- Keys --------------------------------------------------------------

    static void PromptSearch(Editor &e, MailSession &s) {
        const int buffer_id = s.buffer_id;
        const mail::Account *a = AccountAt(e, s.account);
        const bool gmail = a && a->provider == "gmail";
        e.BeginPromptNative(gmail ? "Search mail (Gmail syntax: from:x has:attachment ...)" : "Search mail", s.list.search,
                            [&e, buffer_id](const std::string &q) {
                                MailSession *sp = Session(e, buffer_id);
                                if (!sp || mail::Trim(q).empty()) return;
                                Search(e, *sp, mail::Trim(q));
                            });
    }

    static bool IsEditingKey(int cp) {
        // Keys that would edit the generated text; swallowed (the text is
        // regenerated anyway, but an edit would flash and mark it modified).
        static const char kEdit[] = "iIaAoOsSCDxXrRpPJ~u.><=&";
        return cp < 128 && std::strchr(kEdit, cp) != nullptr && cp != 0;
    }

    static bool HandleKey(Editor &e, int cp, int count) {
        MailSession *sp = Session(e, e.CurPane().buffer_id);
        if (!sp) return false;
        MailSession &s = *sp;
        if (cp == 27) {
            if (s.pending_d) {
                s.pending_d = false;
                return true;
            }
            if (s.picking) {
                s.picking = false;
                s.view = MailSession::View::List;
                Render(e, s);
                SetCursorRow(e, s, s.list_row);
                return true;
            }
            return false;
        }
        if (s.pending_d) {
            s.pending_d = false;
            if (cp == 'd') {
                // 3dd: the count came with the first d (as in Vim).
                MoveMessages(e, s, Targets(e, s, std::max(count, s.pending_d_count)), "trash", "");
                return true;
            }
            // anything else cancels the d and is handled on its own below
        }
        if (cp == '?') {
            e.BeginPreview("Mail keys", kHelpText);
            return true;
        }
        // A typed Enter (:normal, a macro) does what the Enter key does.
        if (cp == '\r' || cp == '\n') return MailActivateRow(e, s);
        if (cp == 'c' || cp == 'm') {
            mail::Draft d;
            const mail::Account *a = AccountAt(e, s.account);
            if (a && !a->signature.empty()) d.body = "\n" + Signature(*a);
            Compose(e, s.account, d, {}, false);
            return true;
        }
        switch (s.view) {
            case MailSession::View::Folders:
                if (cp == 'l' || cp == 'o') return MailActivateRow(e, s);
                if (cp == '$') {
                    Refresh(e, s, false);
                    return true;
                }
                if (cp == 'q' || cp == 'h' || cp == '-') {
                    if (s.picking) {
                        s.picking = false;
                        s.view = MailSession::View::List;
                        Render(e, s);
                        SetCursorRow(e, s, s.list_row);
                    } else if (cp == 'q') {
                        e.BufferDeleteById(s.buffer_id, true);  // MailTeardown runs from there
                        e.SyncModeToActivePaneBuffer();
                    }
                    return true;
                }
                return IsEditingKey(cp) || cp == 'd';
            case MailSession::View::List:
                switch (cp) {
                    case 'l':
                    case 'o': return MailActivateRow(e, s);
                    case 'h':
                    case '-':
                    case 'q': ShowFolders(e, s); return true;
                    case 'r': Reply(e, s, false); return true;
                    case 'R': Reply(e, s, true); return true;
                    case 'f': Forward(e, s); return true;
                    case 'd':
                        s.pending_d = true;
                        s.pending_d_count = count;
                        return true;
                    case 'x': MoveMessages(e, s, Targets(e, s, count), "trash", ""); return true;
                    case 'a': MoveMessages(e, s, Targets(e, s, count), "archive", ""); return true;
                    case 'M': StartMovePick(e, s, count); return true;
                    case 's': ToggleFlag(e, s, count, "\\Flagged"); return true;
                    case 'U': ToggleFlag(e, s, count, "\\Seen"); return true;
                    case '$': Refresh(e, s, false); return true;
                    case '+': LoadMore(e, s); return true;
                    case 'S': PromptSearch(e, s); return true;
                    case 'A': {
                        if (e.mail_.accounts.size() < 2) {
                            e.Notify("Mail: only one account (add more with :MailSetup)", Editor::NotifyLevel::Info);
                            return true;
                        }
                        const int next = (s.account + 1) % static_cast<int>(e.mail_.accounts.size());
                        OpenMailbox(e, s, next, "INBOX");
                        e.SetStatusMessage("Mail: " + e.mail_.accounts[static_cast<size_t>(next)].email);
                        return true;
                    }
                    default: return IsEditingKey(cp);
                }
            case MailSession::View::Message:
                switch (cp) {
                    case 'q':
                    case '-': BackToList(e, s); return true;
                    case 'J': StepMessage(e, s, 1); return true;
                    case 'K': StepMessage(e, s, -1); return true;
                    case 'r': Reply(e, s, false); return true;
                    case 'R': Reply(e, s, true); return true;
                    case 'f': Forward(e, s); return true;
                    case 'd': s.pending_d = true; return true;
                    case 'x': MoveMessages(e, s, Targets(e, s, 1), "trash", ""); return true;
                    case 'a': MoveMessages(e, s, Targets(e, s, 1), "archive", ""); return true;
                    case 'M': StartMovePick(e, s, 1); return true;
                    case 's': ToggleFlag(e, s, 1, "\\Flagged"); return true;
                    case 'U': ToggleFlag(e, s, 1, "\\Seen"); return true;
                    default: return IsEditingKey(cp);
                }
        }
        return false;
    }

    static bool MailActivateRow(Editor &e, MailSession &s) {
        const int row = CursorRow(e, s);
        if (row < 0 || row >= static_cast<int>(s.rows.size())) return s.view != MailSession::View::Message;
        const MailRow r = s.rows[static_cast<size_t>(row)];
        switch (r.kind) {
            case MailRow::Kind::Account:
                if (s.picking) return true;
                OpenMailbox(e, s, r.account, "INBOX");
                return true;
            case MailRow::Kind::Folder: {
                std::string raw = "INBOX";
                const mail::Account *a = AccountAt(e, r.account);
                if (a && r.index >= 0) {
                    auto it = e.mail_.folders.find(a->id);
                    if (it != e.mail_.folders.end() && r.index < static_cast<int>(it->second.size())) {
                        raw = it->second[static_cast<size_t>(r.index)].raw;
                    }
                }
                s.folders_row = row;
                if (s.picking) FinishMovePick(e, s, raw);
                else OpenMailbox(e, s, r.account, raw);
                return true;
            }
            case MailRow::Kind::Draft: {
                MailDraftContext ctx;
                ctx.return_buffer = s.buffer_id;
                const std::string key = AbsKey(r.path);
                if (const MailDraftContext *old = DraftContext(e, key)) {
                    ctx = *old;
                    ctx.return_buffer = s.buffer_id;
                }
                e.mail_.drafts[key] = ctx;
                s.folders_row = row;
                e.LoadFile(r.path);
                DecorateDraftBuffer(e);
                return true;
            }
            case MailRow::Kind::Message: OpenMessage(e, s, r.index); return true;
            case MailRow::Kind::LoadMore: LoadMore(e, s); return true;
            case MailRow::Kind::Attachment: SaveAttachment(e, s, r.index); return true;
            case MailRow::Kind::None: break;
        }
        return s.view != MailSession::View::Message;
    }

    // --- Setup -------------------------------------------------------------

    struct SetupState {
        mail::Account account;
        std::string secret;
    };

    static void ParseHostPort(const std::string &in, std::string *host, int *port, int default_port) {
        std::string s = mail::Trim(in);
        const size_t colon = s.rfind(':');
        *port = default_port;
        if (colon != std::string::npos && colon + 1 < s.size()) {
            *port = std::atoi(s.c_str() + colon + 1);
            s = s.substr(0, colon);
        }
        *host = s;
    }

    static void SetupAskPassword(Editor &e, std::shared_ptr<SetupState> st) {
        const mail::Provider *p = mail::ProviderById(st->account.provider);
        std::string title = "Password for " + st->account.email;
        if (p) title = p->label + " app password for " + st->account.email + "  (" + p->password_help + ")";
        e.BeginPromptNative(
            title, "",
            [&e, st](const std::string &pw) {
                std::string secret = pw;
                // Google shows app passwords in groups of four; they work without the spaces.
                if (st->account.provider == "gmail") secret.erase(std::remove(secret.begin(), secret.end(), ' '), secret.end());
                if (secret.empty()) {
                    e.Notify("Mail setup cancelled: no password given", Editor::NotifyLevel::Warn);
                    return;
                }
                st->secret = secret;
                SetupFinish(e, st);
            },
            /*masked=*/true);
    }

    static void SetupAskName(Editor &e, std::shared_ptr<SetupState> st) {
        e.BeginPromptNative("Your name (shown as the sender)", st->account.name, [&e, st](const std::string &name) {
            st->account.name = mail::Trim(name);
            SetupAskPassword(e, st);
        });
    }

    static void SetupFinish(Editor &e, std::shared_ptr<SetupState> st) {
        mail::Account &a = st->account;
#if !defined(__APPLE__) && !defined(_WIN32)
        if (a.secret == "keychain" && mail::KeychainName() == "a private file") a.secret = "file";
#endif
        // Replace an account with the same address, else add it.
        int idx = AccountIndexForEmail(e, a.email);
        if (idx >= 0) {
            a.id = e.mail_.accounts[static_cast<size_t>(idx)].id;
            const mail::Account &old = e.mail_.accounts[static_cast<size_t>(idx)];
            a.signature = old.signature;
            a.trash = old.trash;
            a.archive = old.archive;
            a.sent = old.sent;
            a.drafts = old.drafts;
            e.mail_.accounts[static_cast<size_t>(idx)] = a;
        } else {
            a.id = mail::AccountIdFor(a.email, e.mail_.accounts);
            e.mail_.accounts.push_back(a);
            idx = static_cast<int>(e.mail_.accounts.size()) - 1;
        }
        SaveAccounts(e);
        e.mail_.accounts_stamp = FileStamp(mail::AccountsPath());  // our own write is not a hand edit
        e.mail_.secrets[a.id] = st->secret;
        e.mail_.folders.erase(a.id);
        e.Notify("Mail: checking " + a.email + "...", Editor::NotifyLevel::Info);
        auto res = std::make_shared<std::pair<std::vector<mail::Folder>, std::string>>();
        auto stored_err = std::make_shared<std::string>();
        auto stored = std::make_shared<bool>(false);
        Editor *ed = &e;
        JobManager::Callbacks cb;
        const mail::Account acct = a;
        const std::string secret = st->secret;
        cb.on_exit = [ed, acct, res, stored, stored_err](int) {
            if (!*stored) {
                ed->Notify("Mail: the password was not saved (" + *stored_err + ") -- it is kept until mep exits",
                           Editor::NotifyLevel::Warn);
            }
            if (!res->second.empty()) {
                ed->Notify("Mail: " + acct.email + ": " + res->second + "  (run :MailSetup again to fix it)",
                           Editor::NotifyLevel::Error);
                ed->mail_.secrets.erase(acct.id);
                mep::NoteActivity();
                return;
            }
            CacheFolders(*ed, acct.id, res->first);
            ed->Notify("Mail: " + acct.email + " is set up (" + Plural(res->first.size(), "folder") + ")",
                       Editor::NotifyLevel::Info);
            const int buf = WorkspaceMailBuffer(*ed);
            if (buf >= 0) {
                if (MailSession *s = Session(*ed, buf)) {
                    if (s->view == MailSession::View::Folders) Render(*ed, *s);
                }
            } else {
                ed->OpenMailInPlace(acct.id);
            }
            mep::NoteActivity();
        };
        JobManager::Instance().SpawnTask(
            [acct, secret, res, stored, stored_err](const Job::EmitLine &, const std::atomic<bool> &cancel) {
                *stored = mail::StoreSecret(acct, secret, stored_err.get());
                mail::ListFolders({acct, secret}, &res->first, &res->second, &cancel);
                return 0;
            },
            std::move(cb), "mail: setup");
    }
};

// --- Editor entry points ----------------------------------------------------

bool Editor::IsMailBuffer(int buffer_id) const { return mail_.sessions.find(buffer_id) != mail_.sessions.end(); }

std::string Editor::MailPaneTitle(int buffer_id) const {
    auto it = mail_.sessions.find(buffer_id);
    if (it == mail_.sessions.end()) return "";
    return it->second.title.empty() ? std::string("Mail") : "Mail: " + it->second.title;
}

bool Editor::MailHandleKey(int cp, int count) { return MailUi::HandleKey(*this, cp, count); }

bool Editor::MailActivate() {
    MailSession *s = MailUi::Session(*this, CurPane().buffer_id);
    if (!s) return false;
    return MailUi::MailActivateRow(*this, *s);
}

void Editor::OpenMailInPlace(const std::string &args) {
    MailUi::EnsureAccounts(*this);
    if (!mail_.accounts_error.empty()) {
        Notify("Mail: " + mail_.accounts_error, NotifyLevel::Error);
        LoadFile(mail::AccountsPath());
        return;
    }
    if (mail_.accounts.empty()) {
        Notify("Mail: no account yet -- let's add one", NotifyLevel::Info);
        MailSetup();
        return;
    }
    if (!mail::CurlAvailable()) {
        Notify("Mail needs `curl` on PATH (it carries IMAP/SMTP over TLS)", NotifyLevel::Error);
        return;
    }
    if (mail_.ns < 0) mail_.ns = CreateNamespace("mail");
    // `:Mail [account] [folder]`
    std::istringstream in(args);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    int account = -1;
    if (!words.empty()) {
        for (size_t i = 0; i < mail_.accounts.size(); i++) {
            if (mail_.accounts[i].id == words[0] || mail::Lower(mail_.accounts[i].email) == mail::Lower(words[0])) {
                account = static_cast<int>(i);
            }
        }
        if (account >= 0) words.erase(words.begin());
    }
    std::string folder;
    for (const std::string &w : words) folder += (folder.empty() ? "" : " ") + w;

    int buffer_id = MailUi::WorkspaceMailBuffer(*this);
    const bool fresh = buffer_id < 0;
    if (fresh) {
        buffer_id = CreateEmptyBuffer();
        MailSession s;
        s.buffer_id = buffer_id;
        s.account = account >= 0 ? account : 0;
        mail_.sessions[buffer_id] = std::move(s);
    }
    MailUi::ShowBufferHere(*this, buffer_id);
    MailSession &s = mail_.sessions[buffer_id];
    if (account < 0) account = s.account;
    if (!folder.empty()) {
        // A folder named as shown (decoded); matched against the cached list.
        std::string raw = mail::EncodeMailboxName(folder);
        const mail::Account &a = mail_.accounts[static_cast<size_t>(account)];
        auto it = mail_.folders.find(a.id);
        if (it != mail_.folders.end()) {
            for (const mail::Folder &f : it->second) {
                if (mail::Lower(f.name) == mail::Lower(folder)) raw = f.raw;
            }
        }
        if (mail::Lower(folder) == "inbox") raw = "INBOX";
        MailUi::OpenMailbox(*this, s, account, raw);
    } else if (fresh || account != s.account) {
        MailUi::OpenMailbox(*this, s, account, "INBOX");
    } else {
        MailUi::Render(*this, s);
        MailUi::SetCursorRow(*this, s, s.view == MailSession::View::List ? s.list_row : 0);
    }
}

void Editor::MailSetup() {
    MailUi::EnsureAccounts(*this);
    if (mail_.ns < 0) mail_.ns = CreateNamespace("mail");
    auto st = std::make_shared<MailUi::SetupState>();
    BeginPromptNative("Email address to set up (Gmail, Outlook, iCloud, Yahoo, Fastmail... or any IMAP server)", "",
                      [this, st](const std::string &email_in) {
                          const std::string email = mail::Trim(email_in);
                          if (email.find('@') == std::string::npos) {
                              Notify("Mail setup: \"" + email + "\" is not an email address", NotifyLevel::Warn);
                              return;
                          }
                          mail::Account &a = st->account;
                          a.email = email;
                          const int existing = MailUi::AccountIndexForEmail(*this, email);
                          if (existing >= 0) a = mail_.accounts[static_cast<size_t>(existing)];
                          a.email = email;
                          const mail::Provider *p = mail::ProviderForEmail(email);
                          if (p) {
                              a.provider = p->id;
                              a.imap_host.clear();
                              a.smtp_host.clear();
                              mail::ApplyProviderDefaults(&a);
                              MailUi::SetupAskName(*this, st);
                              return;
                          }
                          // An unknown domain: ask for its servers.
                          const std::string domain = email.substr(email.find('@') + 1);
                          a.provider = "custom";
                          BeginPromptNative(
                              "IMAP server (host or host:port; 993 = TLS, 143 = STARTTLS)",
                              a.imap_host.empty() ? "imap." + domain : a.imap_host + ":" + std::to_string(a.imap_port),
                              [this, st, domain](const std::string &imap) {
                                  MailUi::ParseHostPort(imap, &st->account.imap_host, &st->account.imap_port, 993);
                                  mail::Account &acc = st->account;
                                  BeginPromptNative(
                                      "SMTP server (host or host:port; 465 = TLS, 587 = STARTTLS)",
                                      acc.smtp_host.empty() ? "smtp." + domain + ":465"
                                                            : acc.smtp_host + ":" + std::to_string(acc.smtp_port),
                                      [this, st](const std::string &smtp) {
                                          MailUi::ParseHostPort(smtp, &st->account.smtp_host, &st->account.smtp_port, 465);
                                          MailUi::SetupAskName(*this, st);
                                      });
                              });
                      });
}

void Editor::MailComposeCommand(const std::string &args) {
    MailUi::EnsureAccounts(*this);
    if (mail_.accounts.empty()) {
        Notify("Mail: set up an account first (:MailSetup)", NotifyLevel::Warn);
        return;
    }
    int account = 0;
    if (MailSession *s = MailUi::Session(*this, CurPane().buffer_id)) account = s->account;
    mail::Draft d;
    d.to = mail::Trim(args);
    const mail::Account &a = mail_.accounts[static_cast<size_t>(account)];
    if (!a.signature.empty()) d.body = "\n" + MailUi::Signature(a);
    MailUi::Compose(*this, account, d, {}, !d.to.empty());
}

void Editor::MailAttach(const std::string &path_in) {
    const int buffer_id = CurPane().buffer_id;
    if (mail::Trim(path_in).empty()) {
        BeginPromptNative("Attach file", "~/", [this](const std::string &p) {
            if (!mail::Trim(p).empty()) MailAttach(p);
        });
        return;
    }
    const std::string path = ExpandPath(path_in);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        Notify("Mail: no such file: " + path, NotifyLevel::Error);
        return;
    }
    Buffer &b = Buf();
    // After the last header line (the header block ends at the first empty line).
    size_t at = 0;
    while (at < b.lines.size() && !b.lines[at].empty() && b.lines[at].find(':') != std::string::npos) at++;
    if (at == 0) {
        Notify("Mail: this buffer is not a draft (no From:/To: header lines)", NotifyLevel::Warn);
        return;
    }
    PushUndoForBuffer(buffer_id);
    b.lines.insert(b.lines.begin() + static_cast<long>(at), "Attach: " + path);
    b.modified = true;
    SetStatusMessage("Attached " + path + " (" + mail::HumanSize(std::filesystem::file_size(path, ec)) + ")");
}

void Editor::MailDiscard() {
    const int buffer_id = CurPane().buffer_id;
    const std::string key = MailUi::DraftKey(*this, buffer_id);
    const std::string drafts = DraftsDir();
    if (key.empty() || drafts.empty() || key.rfind(AbsKey(drafts), 0) != 0) {
        Notify("Mail: this buffer is not a mail draft", NotifyLevel::Warn);
        return;
    }
    BeginConfirmNative(
        "Discard this draft?", false,
        [this, buffer_id, key](bool yes) {
            if (!yes) return;
            MailUi::CloseDraft(*this, buffer_id, key);
            SetStatusMessage("Draft discarded");
        },
        true);
}

void Editor::MailSendCurrent(bool no_confirm) {
    MailUi::EnsureAccounts(*this);
    const int buffer_id = CurPane().buffer_id;
    if (IsMailBuffer(buffer_id)) {
        Notify("Mail: :MailSend sends the draft you are editing -- press c to start one", NotifyLevel::Warn);
        return;
    }
    if (mail_.sending) {
        Notify("Mail: still sending the previous message", NotifyLevel::Warn);
        return;
    }
    const mail::Draft d = mail::ParseDraft(Buf().lines);
    const std::vector<std::string> rcpts = mail::DraftRecipients(d);
    if (rcpts.empty()) {
        Notify("Mail: no recipients -- fill in the To: line", NotifyLevel::Warn);
        return;
    }
    const std::string key = MailUi::DraftKey(*this, buffer_id);
    const MailDraftContext *ctx = MailUi::DraftContext(*this, key);
    // Which account sends it: the one whose address is on the From: line.
    int account = -1;
    const std::vector<mail::Address> from = mail::ParseAddressList(d.from);
    if (!from.empty()) account = MailUi::AccountIndexForEmail(*this, from[0].email);
    if (account < 0 && ctx) account = MailUi::AccountIndexById(*this, ctx->account_id);
    if (account < 0 && from.empty() && !mail_.accounts.empty()) account = 0;
    if (account < 0) {
        Notify(mail_.accounts.empty() ? "Mail: set up an account first (:MailSetup)"
                                      : "Mail: the From: address is not one of your accounts",
               NotifyLevel::Error);
        return;
    }
    std::vector<std::string> files;
    for (const std::string &p : d.attach) {
        const std::string path = ExpandPath(p);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            Notify("Mail: attachment not found: " + path, NotifyLevel::Error);
            return;
        }
        files.push_back(path);
    }
    // Keep what was written, whatever happens next.
    if (!key.empty()) {
        WriteFileBytes(key, JoinDraftLines(Buf().lines));
        Buf().modified = false;
    }
    std::string who;
    for (size_t i = 0; i < rcpts.size() && i < 3; i++) who += (i ? ", " : "") + rcpts[i];
    if (rcpts.size() > 3) who += " and " + std::to_string(rcpts.size() - 3) + " more";
    std::string question = "Send ";
    question += d.subject.empty() ? "this message (no subject!)" : "\"" + d.subject + "\"";
    question += " to " + who;
    if (!files.empty()) question += " with " + Plural(files.size(), "attachment");
    question += "?";
    MailDraftContext context = ctx ? *ctx : MailDraftContext{};
    auto send = [this, buffer_id, key, d, rcpts, files, account, context](bool yes) {
        if (!yes) return;
        auto cred = MailUi::MakeCred(*this, account);
        std::vector<mail::Folder> folders;
        if (auto it = mail_.folders.find(cred->account.id); it != mail_.folders.end()) folders = it->second;
        struct SendResult {
            std::string error;
            std::string warning;
        };
        auto res = std::make_shared<SendResult>();
        mail_.sending = true;
        SetStatusMessage("Mail: sending...");
        JobManager::Callbacks cb;
        cb.on_exit = [this, cred, res, buffer_id, key, context](int) {
            mail_.sending = false;
            MailUi::AfterCred(*this, *cred, res->error);
            if (!res->error.empty()) {
                Notify("Mail: not sent -- " + res->error + " (the draft is kept)", NotifyLevel::Error);
                mep::NoteActivity();
                return;
            }
            Notify("Mail: sent", NotifyLevel::Info);
            if (!res->warning.empty()) Notify("Mail: " + res->warning, NotifyLevel::Warn);
            // The reply's original shows as answered.
            if (context.reply_uid != 0) {
                for (auto &kv : mail_.sessions) {
                    for (mail::MessageSummary &m : kv.second.list.msgs) {
                        if (m.uid == context.reply_uid && kv.second.list.mailbox == context.mailbox) m.SetFlag("\\Answered", true);
                    }
                }
            }
            if (!key.empty()) MailUi::CloseDraft(*this, buffer_id, key);
            SetStatusMessage("Message sent");
            mep::NoteActivity();
        };
        JobManager::Instance().SpawnTask(
            [cred, res, d, rcpts, files, folders, context](const Job::EmitLine &, const std::atomic<bool> &cancel) {
                if (!cred->Ensure()) {
                    res->error = cred->error;
                    return 0;
                }
                std::vector<mail::OutgoingFile> out;
                for (const std::string &path : files) {
                    mail::OutgoingFile f;
                    if (!ReadFileBytes(path, &f.data)) {
                        res->error = "cannot read " + path;
                        return 0;
                    }
                    f.filename = std::filesystem::path(path).filename().string();
                    f.content_type = mail::GuessContentType(f.filename);
                    out.push_back(std::move(f));
                }
                mail::Draft draft = d;
                if (mail::Trim(draft.from).empty()) draft.from = cred->account.FromLine();
                const long long now = static_cast<long long>(std::time(nullptr));
                const std::string message = mail::BuildMessage(draft, out, now, mail::NewMessageId(cred->account.email, now));
                const mail::Session sess = cred->Session();
                if (!mail::SendMail(sess, cred->account.email, rcpts, message, &res->error, &cancel)) return 0;
                // A copy in Sent, where the server does not file one itself.
                if (!cred->account.server_saves_sent) {
                    std::vector<mail::Folder> fs = folders;
                    std::string sent = FindRoleFolder(cred->account, fs, "sent");
                    if (sent.empty() && fs.empty()) {
                        std::string ignored;
                        mail::ListFolders(sess, &fs, &ignored, &cancel);
                        sent = FindRoleFolder(cred->account, fs, "sent");
                    }
                    std::string err;
                    if (sent.empty()) {
                        res->warning = "sent, but no Sent folder was found to keep a copy in";
                    } else if (!mail::AppendMessage(sess, sent, message, &err, &cancel)) {
                        res->warning = "sent, but the copy to " + sent + " failed: " + err;
                    }
                }
                if (context.reply_uid != 0) {
                    std::string ignored;
                    mail::StoreFlags(sess, context.mailbox, {context.reply_uid}, "+FLAGS", "(\\Answered)", &ignored, &cancel);
                }
                return 0;
            },
            std::move(cb), "mail: send");
    };
    if (no_confirm) send(true);
    else BeginConfirmNative(question, true, send);
}

void Editor::MailTick() {
    if (mail_.sessions.empty()) return;
    const double now = gfx::GetTime();
    std::vector<int> due;
    for (auto &kv : mail_.sessions) {
        const MailSession &s = kv.second;
        if (s.view != MailSession::View::List || !s.list.loaded || s.loading > 0 || !s.list.search.empty()) continue;
        if (now - s.last_refresh < kAutoRefreshSec || !IsBufferOnScreen(kv.first)) continue;
        due.push_back(kv.first);
    }
    for (int id : due) {
        if (MailSession *s = MailUi::Session(*this, id)) MailUi::Refresh(*this, *s, true);
    }
}

void Editor::MailTeardown(int buffer_id) {
    auto it = mail_.sessions.find(buffer_id);
    if (it == mail_.sessions.end()) return;
    for (int job : it->second.jobs) JobManager::Instance().Kill(job);
    mail_.sessions.erase(it);
}

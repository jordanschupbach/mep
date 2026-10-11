#ifndef MEP_MAIL_NET_H
#define MEP_MAIL_NET_H

// The email client's network half: IMAP and SMTP over a `curl`
// subprocess, plus the OS credential store. Every function here BLOCKS --
// editor_mail.cpp calls them from JobManager tasks (background threads),
// never from the frame loop.
//
// Why curl: mail needs TLS, and mep deliberately has no TLS stack of its
// own (http_client.h; WEBKIT_PARITY_PLAN.md). curl ships with macOS,
// Windows 10+ and every Linux distribution, and speaks IMAP(S)/SMTP(S)
// with certificate checking, STARTTLS, PLAIN/LOGIN/XOAUTH2 auth and
// connection reuse across the transfers of one invocation. Two details
// shape how it is driven:
//
//   * Secrets never reach argv (where `ps` would show them): every
//     invocation reads its URLs, requests and credentials as a config
//     file on stdin (`-K -`).
//   * curl prints nothing for most custom IMAP commands (FETCH, SEARCH,
//     STORE...), so those run under `--trace -`, whose hex dump of the
//     received protocol bytes is exact (literals and 8-bit data
//     included) and is parsed by mail::ParseCurlTrace. Whole messages
//     come through curl's own `;UID=n` fetch instead, which frames the
//     literal by its byte count and writes it to stdout verbatim.

#include "mail.h"

#include <atomic>
#include <string>
#include <vector>

namespace mail {

struct ProcResult {
    bool spawned = false;
    bool timed_out = false;
    bool cancelled = false;
    int exit_code = -1;
    std::string out;
    std::string err;
};

/**
 * @brief Runs `argv` (PATH lookup, no shell), feeding `stdin_data` and collecting stdout/stderr.
 * @param argv Program and arguments.
 * @param stdin_data Written to the child's stdin, which is then closed.
 * @param timeout_ms The child is killed after this long.
 * @param cancel Polled; the child is killed once it reads true. May be null.
 */
ProcResult RunProcess(const std::vector<std::string> &argv, const std::string &stdin_data, int timeout_ms,
                      const std::atomic<bool> *cancel);

/** @brief mep's mail directory (accounts.json, drafts/, secrets/), created on demand; empty if unknown. */
std::string MailDataDir();
/** @brief The accounts file path. */
std::string AccountsPath();
/** @brief Whether curl is runnable. */
bool CurlAvailable();

// An account plus its resolved password/token, as handed to a worker.
struct Session {
    Account account;
    std::string secret;
};

/**
 * @brief Looks up the account's password (or OAuth2 token) per Account::secret.
 * @return The secret; empty with `*error` set when there is none.
 */
std::string ResolveSecret(const Account &a, std::string *error);
/** @brief Saves `secret` for `a` in the store Account::secret names ("command" accounts store nothing). */
bool StoreSecret(const Account &a, const std::string &secret, std::string *error);
/** @brief Removes a stored secret (best effort). */
void ForgetSecret(const Account &a);
/** @brief Where Account::secret == "keychain" actually puts things on this platform, for messages. */
std::string KeychainName();

// One IMAP transfer of an invocation: `request` is a custom command run
// after curl SELECTs `mailbox` (when non-empty); with an empty request the
// URL itself is fetched (`url_suffix` like ";UID=42").
struct ImapStep {
    std::string mailbox;  // raw (modified UTF-7) name; empty = no SELECT
    std::string request;
    std::string url_suffix;
};

struct ImapResult {
    bool ok = false;
    std::string error;
    CurlTrace trace;
    std::vector<ImapResponse> responses;
    std::string body;  // stdout of a plain URL fetch
};

/**
 * @brief Runs several IMAP steps over one connection. Traced (custom-command) or plain (fetch) -- not both.
 */
ImapResult RunImap(const Session &s, const std::vector<ImapStep> &steps, bool traced, const std::atomic<bool> *cancel,
                   int timeout_ms = 120000);

bool ListFolders(const Session &s, std::vector<Folder> *out, std::string *error, const std::atomic<bool> *cancel);
bool SelectMailbox(const Session &s, const std::string &mailbox, SelectInfo *out, std::string *error,
                   const std::atomic<bool> *cancel);
/** @brief FETCHes summaries for `set` -- sequence numbers, or UIDs when `by_uid`. */
bool FetchSummaries(const Session &s, const std::string &mailbox, const std::string &set, bool by_uid,
                    std::vector<MessageSummary> *out, std::string *error, const std::atomic<bool> *cancel);
/** @brief The whole raw message (marks it \\Seen on the server, as reading it should). */
bool FetchMessage(const Session &s, const std::string &mailbox, uint32_t uid, std::string *raw, std::string *error,
                  const std::atomic<bool> *cancel);
/** @brief UID STORE: `op` is "+FLAGS" or "-FLAGS", `flags` like "(\\Seen)". */
bool StoreFlags(const Session &s, const std::string &mailbox, const std::vector<uint32_t> &uids, const std::string &op,
                const std::string &flags, std::string *error, const std::atomic<bool> *cancel);
/** @brief Moves messages (UID MOVE; COPY + \\Deleted + EXPUNGE where MOVE is missing). */
bool MoveMessages(const Session &s, const std::string &mailbox, const std::vector<uint32_t> &uids,
                  const std::string &dest, std::string *error, const std::atomic<bool> *cancel);
/** @brief UID SEARCH with a free-text query (Gmail: X-GM-RAW, i.e. the web search syntax). */
bool SearchMailbox(const Session &s, const std::string &mailbox, const std::string &query, std::vector<uint32_t> *uids,
                   std::string *error, const std::atomic<bool> *cancel);
/** @brief Sends `message` (CRLF RFC 5322) from `from_email` to `recipients` over SMTP. */
bool SendMail(const Session &s, const std::string &from_email, const std::vector<std::string> &recipients,
              const std::string &message, std::string *error, const std::atomic<bool> *cancel);
/** @brief APPENDs `message` to `mailbox` (marked \\Seen) -- the Sent copy on servers that do not keep one. */
bool AppendMessage(const Session &s, const std::string &mailbox, const std::string &message, std::string *error,
                   const std::atomic<bool> *cancel);

/** @brief A readable reason for a curl exit code (plus the server's own words when the trace has them). */
std::string DescribeCurlFailure(int exit_code, const std::string &stderr_text, const Account &a);

}  // namespace mail

#endif  // MEP_MAIL_NET_H

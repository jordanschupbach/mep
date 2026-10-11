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
/** @brief Saves `secret` for `a` in the store Account::secret names ("command" accounts store nothing).
 *  `interactive`: the user is at hand (setup, sign-in), so macOS may ask permission -- a background save
 *  never shows a dialog and just fails instead. */
bool StoreSecret(const Account &a, const std::string &secret, std::string *error, bool interactive = true);
// --- The vault -----------------------------------------------------------
//
// Account::secret == "vault": the secret lives in MailDataDir()/vault.json,
// encrypted with ChaCha20-Poly1305 under a key derived from the user's
// master passphrase (PBKDF2-HMAC-SHA256, 600k iterations). The key is held
// in memory only while the vault is unlocked; mep never writes it, or the
// passphrase, anywhere. Used where the OS has no credential store (Linux
// without a Secret Service keyring), and the destination for secrets that
// older versions kept in plain files.

/** @brief Whether a vault file exists yet. */
bool VaultExists();
/** @brief Whether the vault's key is in memory. */
bool VaultUnlocked();
/**
 * @brief Derives the key from `passphrase` and opens the vault -- or, with `create` and no vault yet, makes an
 *        empty one. Slow on purpose (the key derivation): call from a worker thread.
 * @return False with `*error` set for a wrong passphrase or an unreadable/tampered file.
 */
bool VaultUnlock(const std::string &passphrase, bool create, std::string *error);
/** @brief Forgets the key and the decrypted secrets. */
void VaultLock();
/** @brief Drops every cached access token (with VaultLock, :MailLock). */
void ForgetAllAccessTokens();

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

// --- OAuth2 browser sign-in (mail.h's OAuthProvider) ---------------------

/** @brief Opens `url` in the user's browser: $BROWSER if set, else the platform's opener. */
bool OpenInBrowser(const std::string &url, std::string *error);

// A one-shot HTTP listener on 127.0.0.1 for the sign-in redirect.
class OAuthRedirectListener {
public:
    OAuthRedirectListener() = default;
    ~OAuthRedirectListener();
    OAuthRedirectListener(const OAuthRedirectListener &) = delete;
    OAuthRedirectListener &operator=(const OAuthRedirectListener &) = delete;
    /** @brief Binds an OS-chosen port on 127.0.0.1. */
    bool Listen(std::string *error);
    int Port() const { return port_; }
    /**
     * @brief Serves requests until one carries `code` (or `error`) with the expected `state`.
     * @return The code; empty with `*error` set on refusal, timeout or cancel.
     */
    std::string AwaitCode(const std::string &state, int timeout_ms, const std::atomic<bool> *cancel, std::string *error);

private:
    long long fd_ = -1;
    int port_ = 0;
};

/** @brief Trades an authorization code for tokens at the provider's token endpoint. */
OAuthTokens OAuthExchangeCode(const OAuthProvider &p, const Account &a, const std::string &code,
                              const std::string &verifier, const std::string &redirect_uri, const std::atomic<bool> *cancel);
/** @brief Mints an access token from a refresh token. */
OAuthTokens OAuthRefresh(const OAuthProvider &p, const Account &a, const std::string &refresh_token,
                         const std::atomic<bool> *cancel);

/**
 * @brief What an account logs in with, from its stored secret: the password itself, or -- for browser
 * sign-in -- a fresh access token minted from the stored refresh token (cached until shortly before it
 * expires). When the provider rotated the refresh token, the new one is stored and returned in
 * `*new_stored`.
 */
std::string LoginSecret(const Account &a, const std::string &stored, std::string *new_stored, std::string *error,
                        const std::atomic<bool> *cancel);
// --- OAuth clients ---------------------------------------------------------
//
// The client a browser sign-in presents is looked up in order: the
// account's own (oauth_client_id in accounts.json), then the one saved for
// the provider in MailDataDir()/oauth_clients.json (what :MailSetup stores
// after importing Google's client JSON or a pasted ID), then the one built
// into this mep (CMake / environment, mail.h's HasBuiltinOAuthClient).

struct OAuthClient {
    std::string id;
    std::string secret;
};

/** @brief The client saved for `provider_id`; false when none is. */
bool LoadSavedOAuthClient(const std::string &provider_id, OAuthClient *out);
/** @brief Saves the client for `provider_id` (owner-only file). */
bool SaveOAuthClient(const std::string &provider_id, const OAuthClient &client, std::string *error);
/** @brief Whether any client (saved or built in) exists for `provider_id`. */
bool HaveOAuthClient(const std::string &provider_id);
/** @brief Fills an account's empty client from the saved one, else the built-in one. */
void FillOAuthClient(Account *a);
/** @brief Reads the JSON Google's console downloads for a client ({"installed": {...}}). */
bool ParseGoogleClientJson(const std::string &text, OAuthClient *out, std::string *error);
/** @brief The newest client_secret_*.json in ~/Downloads, or "". */
std::string FindGoogleClientJson();

/** @brief Appends a line to MailDataDir()/mail.log (owner-only, kept small). Never pass a secret. */
void MailLog(const std::string &line);
/** @brief The log's path. */
std::string MailLogPath();

/** @brief Drops a cached access token (after the server refused it). */
void ForgetAccessToken(const Account &a);

/** @brief A readable reason for a curl exit code (plus the server's own words when the trace has them). */
std::string DescribeCurlFailure(int exit_code, const std::string &stderr_text, const Account &a);

}  // namespace mail

#endif  // MEP_MAIL_NET_H

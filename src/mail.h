#ifndef MEP_MAIL_H
#define MEP_MAIL_H

// The email client's pure half (:Mail, editor_mail.cpp): everything that
// turns bytes into messages and messages into bytes, with no network, no
// editor and no display -- so all of it is unit-tested (mail_test.cpp).
//
//   * encodings: base64, quoted-printable, RFC 2047 encoded words, the
//     handful of single-byte charsets mail still arrives in, IMAP's
//     modified UTF-7 mailbox names;
//   * RFC 5322 / MIME: header blocks, address lists, dates, multipart
//     bodies (best text part, HTML rendered to text, attachments);
//   * composing: the draft buffer's text format, reply/forward drafts and
//     the outgoing MIME message;
//   * IMAP: the response grammar (literals included) and the readers for
//     LIST / SEARCH / SELECT / FETCH answers, plus the `curl --trace`
//     dump that carries them (see mail_net.h for why curl is the
//     transport);
//   * accounts: the provider presets (Gmail, Outlook, ...) and the
//     accounts file.
//
// Strings are bytes; anything meant for display comes out as UTF-8.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mail {

// --- Encodings ----------------------------------------------------------

/** @brief Standard base64 (RFC 4648) with `=` padding; `wrap` > 0 breaks lines (CRLF) every `wrap` chars. */
std::string Base64Encode(std::string_view in, size_t wrap = 0);
/** @brief Decodes base64, skipping whitespace and any non-alphabet byte; stops at padding. */
std::string Base64Decode(std::string_view in);
/** @brief Decodes quoted-printable; `header` also turns `_` into a space (RFC 2047 "Q"). */
std::string QpDecode(std::string_view in, bool header = false);
/** @brief Encodes text as quoted-printable (RFC 2045), CRLF line breaks, soft breaks at 76 columns. */
std::string QpEncode(std::string_view in);
/** @brief Converts `bytes` in `charset` to UTF-8 (UTF-8/ASCII, ISO-8859-1/-15, Windows-1252; anything else is
 *  kept when it is valid UTF-8 and read as Windows-1252 otherwise). Invalid UTF-8 becomes U+FFFD. */
std::string ToUtf8(std::string_view bytes, std::string_view charset);
/** @brief Decodes RFC 2047 encoded words (`=?utf-8?B?...?=`) in a header value; whitespace between two adjacent
 *  encoded words is dropped, as the RFC requires. Raw 8-bit text is taken as UTF-8. */
std::string DecodeHeaderWords(std::string_view value);
/** @brief Encodes a header value for the wire: unchanged when it is printable ASCII, otherwise as UTF-8 "B"
 *  encoded words of at most 75 chars, split on character boundaries. */
std::string EncodeHeaderWords(std::string_view value);
/** @brief IMAP modified UTF-7 (RFC 3501 5.1.3) mailbox name -> UTF-8. */
std::string DecodeMailboxName(std::string_view name);
/** @brief UTF-8 -> IMAP modified UTF-7. */
std::string EncodeMailboxName(std::string_view name);

// --- RFC 5322 -----------------------------------------------------------

struct Header {
    std::string name;   // as written
    std::string value;  // unfolded, still encoded
};

/** @brief Parses a header block (up to the first empty line, if any), unfolding continuation lines. */
std::vector<Header> ParseHeaders(std::string_view block);
/** @brief First header named `name` (case-insensitive), raw; empty if absent. */
std::string HeaderValue(const std::vector<Header> &headers, std::string_view name);

struct Address {
    std::string name;   // decoded display name, may be empty
    std::string email;  // addr-spec
    /** @brief "Name <email>" (or just the email), decoded, for display. */
    std::string Display() const;
    /** @brief The name if there is one, else the email. */
    std::string Short() const;
};

/** @brief Parses an address-list header value: quoted and encoded display names, comments, groups. */
std::vector<Address> ParseAddressList(std::string_view value);
/** @brief Formats addresses for an outgoing header (display names quoted or RFC 2047 encoded). */
std::string FormatAddressList(const std::vector<Address> &addrs);

/** @brief Parses an RFC 5322 date (or IMAP INTERNALDATE "10-Oct-2026 21:15:04 -0600") to Unix time; -1 on failure. */
long long ParseDate(std::string_view value);
/** @brief Formats Unix time as an RFC 5322 date in the local time zone. */
std::string FormatRfc5322Date(long long t);
/** @brief The list column: "14:05" today, "Oct 10" this year, "2024-03-01" otherwise (local time). */
std::string FormatListDate(long long t, long long now);
/** @brief A full human date for the message view: "Sat, 10 Oct 2026 21:15" (local time). */
std::string FormatLongDate(long long t);

// --- MIME ---------------------------------------------------------------

struct Attachment {
    std::string filename;
    std::string content_type;  // lower-cased "type/subtype"
    std::string data;          // decoded bytes
};

struct Message {
    std::vector<Header> headers;
    std::string subject;  // decoded
    std::vector<Address> from, to, cc, reply_to;
    long long date = -1;
    std::string date_raw;
    std::string message_id;  // with angle brackets
    std::string in_reply_to;
    std::string references;
    std::string text;        // the body to read: text/plain, else HTML rendered as text
    bool text_from_html = false;
    std::string html;        // the raw HTML part, when there was one
    std::vector<Attachment> attachments;
};

/** @brief Parses a whole RFC 5322 message (CRLF or LF), walking multipart bodies. */
Message ParseMessage(std::string_view raw);
/** @brief Renders HTML as readable text: block structure kept, scripts/styles dropped, entities decoded, link
 *  targets listed as numbered footnotes ("text [1]" ... "[1] https://..."). */
std::string HtmlToText(std::string_view html);
/** @brief A best-guess media type for a file name ("report.pdf" -> "application/pdf"). */
std::string GuessContentType(std::string_view filename);

// --- Composing ----------------------------------------------------------

// The compose buffer is a header block, an empty line, then the body:
//
//   From: Ada <ada@example.com>
//   To: bob@example.com
//   Cc:
//   Bcc:
//   Subject: Lunch?
//   Attach: ~/notes.pdf        (one per file; optional)
//
//   Body text...
//
// In-Reply-To / References ride along as ordinary header lines on a reply.
struct Draft {
    std::string from, to, cc, bcc, subject;
    std::string in_reply_to, references;
    std::vector<std::string> attach;  // file paths
    std::string body;                 // LF line breaks
};

/** @brief Parses a compose buffer's lines. */
Draft ParseDraft(const std::vector<std::string> &lines);
/** @brief The compose buffer's lines for `d`. */
std::vector<std::string> DraftLines(const Draft &d);
/** @brief "Re: x" for a reply to "x" (not doubled). */
std::string ReplySubject(std::string_view subject);
/** @brief "Fwd: x". */
std::string ForwardSubject(std::string_view subject);
/** @brief A reply draft: To = Reply-To/From; `all` adds the other To/Cc recipients minus `self_email`;
 *  the body quotes the original under an attribution line. */
Draft MakeReplyDraft(const Message &m, const std::string &from_line, const std::string &self_email, bool all);
/** @brief A forward draft with the original's headers and text inline (attachments are the caller's: it has to
 *  write them somewhere to name them in Attach: lines). */
Draft MakeForwardDraft(const Message &m, const std::string &from_line);

struct OutgoingFile {
    std::string filename;
    std::string content_type;
    std::string data;
};

/** @brief Builds the outgoing RFC 5322 message (CRLF), multipart/mixed when there are files. `now` is the Date;
 *  `message_id` must include the angle brackets. Bcc is never written. */
std::string BuildMessage(const Draft &d, const std::vector<OutgoingFile> &files, long long now,
                         const std::string &message_id);
/** @brief Every envelope recipient (To, Cc and Bcc addr-specs), deduplicated. */
std::vector<std::string> DraftRecipients(const Draft &d);
/** @brief A fresh Message-ID "<random.time@domain>" with the domain taken from `from_email`. */
std::string NewMessageId(const std::string &from_email, long long now);

// --- IMAP ---------------------------------------------------------------

// One parsed value of an IMAP response: an atom (numbers, NIL and flags
// too), a string (quoted or literal -- `literal` says which), or a list.
struct ImapValue {
    enum class Kind { Atom, String, List, Nil };
    Kind kind = Kind::Nil;
    std::string text;
    std::vector<ImapValue> items;
    bool IsAtom(std::string_view a) const;
    /** @brief The text of an atom or string; empty for NIL and lists. */
    const std::string &Str() const { return text; }
};

// One response: an untagged "* ..." or a tagged "A001 OK ..." line, with
// its literals spliced in. `tag` is "*" for untagged, "+" for continuation.
struct ImapResponse {
    std::string tag;
    std::vector<ImapValue> values;  // everything after the tag
    std::string line;               // the first line, without CRLF (diagnostics)
};

/** @brief Splits a received IMAP stream into responses, honouring {n} literals. */
std::vector<ImapResponse> ParseImapStream(std::string_view data);

struct Folder {
    std::string raw;      // the server's (modified UTF-7) name, used on the wire
    std::string name;     // decoded, for display
    char delim = '/';
    std::vector<std::string> attrs;  // lower-cased: "\\noselect", "\\trash", ...
    bool selectable = true;
    /** @brief The special-use role ("trash", "sent", "archive", "all", "drafts", "junk", "flagged") or "". */
    std::string role;
};

struct MessageSummary {
    uint32_t uid = 0;
    uint32_t seq = 0;
    std::vector<std::string> flags;  // as sent ("\\Seen", "$Forwarded", ...)
    long long internal_date = -1;
    uint64_t size = 0;
    std::string subject;  // decoded
    std::vector<Address> from, to;
    long long date = -1;   // header Date, else INTERNALDATE
    std::string message_id;
    bool has_attachment = false;  // multipart/mixed (a cheap header-only guess)
    bool Seen() const { return HasFlag("\\Seen"); }
    bool Flagged() const { return HasFlag("\\Flagged"); }
    bool Answered() const { return HasFlag("\\Answered"); }
    bool HasFlag(std::string_view flag) const;
    void SetFlag(std::string_view flag, bool on);
};

struct SelectInfo {
    uint32_t exists = 0;
    uint32_t uidvalidity = 0;
    uint32_t uidnext = 0;
    bool ok = false;
};

/** @brief The folders in LIST responses (special-use roles from attributes, else from well-known names). */
std::vector<Folder> ReadListResponses(const std::vector<ImapResponse> &rs);
/** @brief The message summaries in FETCH responses (UID FLAGS INTERNALDATE RFC822.SIZE BODY[HEADER.FIELDS ...]). */
std::vector<MessageSummary> ReadFetchSummaries(const std::vector<ImapResponse> &rs);
/** @brief The numbers of SEARCH responses. */
std::vector<uint32_t> ReadSearch(const std::vector<ImapResponse> &rs);
/** @brief EXISTS / UIDVALIDITY / UIDNEXT out of a SELECT's untagged responses. */
SelectInfo ReadSelect(const std::vector<ImapResponse> &rs);
/** @brief Quotes `s` as an IMAP quoted string. */
std::string ImapQuote(std::string_view s);
/** @brief "1:5,9,12:13"-style set from a list of numbers (sorted, deduplicated). */
std::string ImapSet(std::vector<uint32_t> nums);
/** @brief The header fields the message list asks for. */
const char *SummaryFetchItems();

// What `curl --trace -` recorded of an IMAP session: the bytes the server
// sent (only the protocol, "Recv header" blocks -- curl also echoes
// matched untagged lines as "Recv data", which would double them) and
// the tagged command lines the client sent. Authentication lines are
// dropped on the way in, so no credential survives the parse.
struct CurlTrace {
    std::string received;
    std::vector<std::string> sent;  // "A004 UID FETCH ..." without CRLF
};

/** @brief Parses `curl --trace -` output. */
CurlTrace ParseCurlTrace(std::string_view trace);
/** @brief The tagged status ("OK", "NO ...", "BAD ...") of the sent command starting with `command_prefix`
 *  (case-insensitive, after the tag), the last one if it was sent more than once; "" if never answered. */
std::string TaggedStatus(const CurlTrace &t, const std::vector<ImapResponse> &rs, std::string_view command_prefix);

// --- Accounts -----------------------------------------------------------

struct Provider {
    std::string id;     // "gmail"
    std::string label;  // "Gmail"
    std::string imap_host;
    int imap_port = 993;
    std::string smtp_host;
    int smtp_port = 465;  // 465 = implicit TLS, 587 = STARTTLS
    bool server_saves_sent = false;  // Gmail files sent mail itself; others need an APPEND
    std::string password_help;       // where an app password comes from
    std::vector<std::string> domains;
};

/** @brief The built-in presets. */
const std::vector<Provider> &Providers();
/** @brief The preset whose domain list holds `email`'s domain; nullptr if none. */
const Provider *ProviderForEmail(std::string_view email);
/** @brief The preset named `id`; nullptr if none. */
const Provider *ProviderById(std::string_view id);

struct Account {
    std::string id;        // short handle, unique ("personal", "work")
    std::string name;      // display name for From:
    std::string email;
    std::string username;  // login; defaults to email
    std::string provider;  // preset id, or "custom"
    std::string imap_host;
    int imap_port = 993;
    std::string smtp_host;
    int smtp_port = 465;
    // How the password (or OAuth2 token) is found:
    //   "keychain" -- the OS credential store (macOS Keychain, Secret Service, Windows DPAPI file)
    //   "command"  -- run `secret_command`, use its first stdout line
    //   "file"     -- a private file in mep's data directory
    std::string secret = "keychain";
    std::string secret_command;
    bool oauth2 = false;          // the secret is an OAuth2 bearer token (XOAUTH2/OAUTHBEARER)
    // Browser sign-in (OAuth2Provider): with a provider set, the stored
    // secret is a refresh token, and access tokens are minted from it.
    std::string oauth_provider;   // "google", "microsoft", or "" for none
    std::string oauth_client_id;
    std::string oauth_client_secret;  // Google's "desktop app" secret (not confidential by design)
    // Endpoint overrides (another provider, or a test server).
    std::string oauth_auth_url, oauth_token_url, oauth_scope;
    bool server_saves_sent = false;
    bool insecure = false;        // plain imap:// / smtp:// -- localhost testing only
    std::string signature;        // appended to new drafts after "-- "
    // Folder overrides; empty = found from the server's special-use flags.
    std::string trash, archive, sent, drafts;

    std::string Login() const { return username.empty() ? email : username; }
    /** @brief True when logins use an OAuth2 access token rather than a password. */
    bool UsesOAuth() const { return oauth2 || !oauth_provider.empty(); }
    /** @brief "Name <email>" for From:. */
    std::string FromLine() const;
};

/** @brief Reads the accounts file (JSON); empty with `*error` set when it is unreadable/invalid,
 *  empty with no error when it does not exist. */
std::vector<Account> LoadAccounts(const std::string &path, std::string *error);
/** @brief Writes the accounts file (owner-only permissions where the platform has them). */
bool SaveAccounts(const std::string &path, const std::vector<Account> &accounts);
/** @brief Fills host/port/flags from the provider preset when the account leaves them empty. */
void ApplyProviderDefaults(Account *a);
/** @brief An account id derived from an email ("jane.doe@x.com" -> "jane.doe"), unique among `existing`. */
std::string AccountIdFor(const std::string &email, const std::vector<Account> &existing);

// --- OAuth2 browser sign-in -----------------------------------------------
//
// The authorization-code flow with PKCE (RFC 7636) and a loopback
// redirect (RFC 8252): mep listens on 127.0.0.1, opens the provider's
// sign-in page in the browser, and receives the code on the redirect --
// the password is only ever typed into the provider's own page.

struct OAuthProvider {
    std::string id;     // "google"
    std::string label;  // "Google"
    std::string auth_url;
    std::string token_url;
    std::string scope;
    std::string redirect_host;  // "127.0.0.1" or "localhost" (what the provider's registration allows)
    std::string extra_params;   // appended to the authorize URL (already encoded)
    bool needs_secret = false;  // asks for a client secret at setup
    std::string console_url;    // where to make a client ID
};

/** @brief The built-in providers ("google", "microsoft"). */
const std::vector<OAuthProvider> &OAuthProviders();
/** @brief The provider named `id`, nullptr if none. */
const OAuthProvider *OAuthProviderById(std::string_view id);
/** @brief The browser sign-in that goes with a mail preset ("gmail" -> google, "outlook" -> microsoft). */
const OAuthProvider *OAuthProviderForMail(std::string_view mail_provider);
/** @brief Whether this build carries its own client for `provider_id` (so setup needs no client ID). The
 *  environment (MEP_GOOGLE_OAUTH_CLIENT_ID / _SECRET, MEP_MICROSOFT_OAUTH_CLIENT_ID) overrides the build's. */
bool HasBuiltinOAuthClient(std::string_view provider_id);
/** @brief Fills an account's empty client ID/secret from the built-in client, if there is one. */
void FillBuiltinOAuthClient(Account *a);
/** @brief The mail preset that goes with a sign-in provider ("google" -> "gmail"). */
std::string MailPresetForOAuth(std::string_view oauth_provider);
/** @brief The account's provider with its endpoint overrides applied; false when it has none. */
bool AccountOAuthProvider(const Account &a, OAuthProvider *out);

/** @brief SHA-256 of `data`, as 32 raw bytes. */
std::string Sha256(std::string_view data);
/** @brief base64url without padding (RFC 4648 section 5). */
std::string Base64Url(std::string_view in);
/** @brief The PKCE S256 challenge for a code verifier. */
std::string PkceChallenge(std::string_view verifier);
/** @brief Percent-encodes for a URL query / form value (unreserved characters kept). */
std::string UrlEncodeComponent(std::string_view s);
/** @brief Decodes %XX and '+' in a query value. */
std::string UrlDecodeComponent(std::string_view s);
/** @brief "a=1&b=2" -> pairs, decoded. */
std::vector<std::pair<std::string, std::string>> ParseQuery(std::string_view query);
/** @brief The browser URL that starts sign-in. */
std::string OAuthAuthorizeUrl(const OAuthProvider &p, const std::string &client_id, const std::string &redirect_uri,
                              const std::string &state, const std::string &challenge, const std::string &login_hint);

struct OAuthTokens {
    std::string email;  // who signed in (the OpenID id_token's email claim)
    std::string name;   // their display name, when shared
    std::string access_token;
    std::string refresh_token;  // empty when the provider did not send a new one
    long long expires_in = 0;
    std::string error;          // "invalid_grant: Token has been expired or revoked." etc.
};

/** @brief The email and name claims of an OpenID id_token (a JWT straight from the token endpoint over TLS,
 *  which OpenID Connect lets a client trust without checking its signature). */
void ParseIdTokenClaims(std::string_view jwt, std::string *email, std::string *name);
/** @brief Reads a token endpoint's JSON answer. */
OAuthTokens ParseTokenResponse(std::string_view json);

// --- curl ---------------------------------------------------------------

/** @brief Quotes a value for a curl config file (`-K`): double quotes with \\ \" \n \r \t escaped. */
std::string CurlConfigQuote(std::string_view s);
/** @brief Percent-encodes one URL path segment (mailbox names keep their '/' hierarchy delimiter only when
 *  `keep_slash`). */
std::string UrlEncodePath(std::string_view s, bool keep_slash);
/** @brief imaps://host:port/ [+ encoded mailbox]. */
std::string ImapUrl(const Account &a, std::string_view mailbox_raw);
/** @brief smtps://host:465 or smtp://host:587 (STARTTLS required by the caller's ssl-reqd). */
std::string SmtpUrl(const Account &a);

// --- Safety checks --------------------------------------------------------

/** @brief True for 127.x.x.x, ::1 and localhost -- where an unencrypted connection never leaves the machine. */
bool IsLoopbackHost(std::string_view host);
/** @brief The host of an http(s) URL ("https://a.b:1/x" -> "a.b"). */
std::string UrlHost(std::string_view url);
/** @brief https://, or http:// to a loopback host (a local test server) -- never plain http over the network. */
bool IsSafeAuthUrl(std::string_view url);
/** @brief A pasted/typed client value cleaned up: surrounding whitespace and quotes removed. */
std::string CleanPastedValue(std::string_view s);
/** @brief Shape check for a Google OAuth client ID ("<digits>-<letters>.apps.googleusercontent.com"). */
bool LooksLikeGoogleClientId(std::string_view id);
/** @brief A plain addr-spec with nothing a command line or keychain query could misread (no spaces, quotes,
 *  control characters). */
bool IsPlainEmail(std::string_view email);

// --- Small helpers shared with the editor side ---------------------------

/** @brief ASCII lower-casing. */
std::string Lower(std::string_view s);
/** @brief Trims ASCII whitespace from both ends. */
std::string Trim(std::string_view s);
/** @brief Display width in terminal cells of UTF-8 text (one cell per code point; wide CJK counts two). */
size_t DisplayWidth(std::string_view s);
/** @brief Truncates/pads UTF-8 `s` to exactly `width` cells (an ellipsis marks a cut). */
std::string FitWidth(std::string_view s, size_t width);
/** @brief "1.2 MB"-style size. */
std::string HumanSize(uint64_t bytes);
/** @brief A filename that is safe to write (no path separators or control characters; never empty). */
std::string SafeFilename(std::string_view name);

}  // namespace mail

#endif  // MEP_MAIL_H

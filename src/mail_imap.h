#ifndef MEP_MAIL_IMAP_H
#define MEP_MAIL_IMAP_H

// mep's own IMAP connections (OpenSSL), kept open between operations.
//
// Every curl run is a fresh connection: TLS, login, and a SELECT before
// the command itself. Servers that hold each command back (Gmail tags its
// answers [THROTTLED] and delays them seconds apiece) make that SELECT the
// most expensive part of opening a folder. Here a logged-in connection
// goes back to a small per-account pool after each operation and keeps
// its selected mailbox, so the next operation on that folder sends only
// its own command.
//
// RunImap (mail_net.cpp) comes here first and falls back to curl when
// this is unavailable (a build without OpenSSL, MEP_MAIL_CURL=1) or the
// TLS handshake itself fails (curl has its own trust store). The result
// is shaped exactly like curl's, trace included, so callers cannot tell.

#include "mail_net.h"

#include <atomic>
#include <string>
#include <vector>

namespace mail {

/** @brief Whether pooled native connections are compiled in and not switched off (MEP_MAIL_CURL=1). */
bool NativeImapAvailable();

/**
 * @brief Runs `steps` over a pooled connection, as RunImap does over curl.
 * @return false when curl should run them instead (the connection could not
 *         be set up for a reason curl might not share); `*r` is then unspecified.
 */
bool RunImapNative(const Session &s, const std::vector<ImapStep> &steps, bool traced, const std::atomic<bool> *cancel,
                   int timeout_ms, ImapResult *r);

/** @brief Logs out of and drops every pooled connection (the saved secrets were forgotten). */
void CloseImapConnections();

}  // namespace mail

#endif  // MEP_MAIL_IMAP_H

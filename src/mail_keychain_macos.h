#ifndef MEP_MAIL_KEYCHAIN_MACOS_H
#define MEP_MAIL_KEYCHAIN_MACOS_H

// macOS only (mail_keychain_macos.cpp): generic-password items through the
// Security framework, so the items trust mep alone.

#include <string>

namespace mail::keychain {

enum class Status { Ok, NotFound, Denied, Error };

/**
 * @brief Reads an item. Without `allow_ui` an item that does not trust mep comes back Denied instead of
 *        showing macOS's "allow access?" dialog. `*ours` says whether mep wrote it (trusts mep alone).
 */
Status Read(const std::string &service, const std::string &account, bool allow_ui, std::string *secret, bool *ours);
/** @brief Whether an item exists and `*ours` (mep wrote it), from its attributes alone -- never a dialog. */
Status Probe(const std::string &service, const std::string &account, bool *ours);
/** @brief Deletes an item (Denied without `allow_ui` when it does not trust mep). */
Status Delete(const std::string &service, const std::string &account, bool allow_ui);
/** @brief Adds an item that trusts this mep only (Denied without `allow_ui` if macOS wanted to ask first). */
Status Add(const std::string &service, const std::string &account, const std::string &label, const std::string &secret,
           bool allow_ui);

}  // namespace mail::keychain

#endif  // MEP_MAIL_KEYCHAIN_MACOS_H

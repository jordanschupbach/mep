#ifndef MEP_FILE_DIALOG_H
#define MEP_FILE_DIALOG_H

// The platform's own "open file" dialog, called directly (NSOpenPanel on
// macOS, the common dialog on Windows) rather than through osascript or
// PowerShell (plans/DEPENDENCY_REMOVAL_PLAN.md). Linux and the web build
// have none mep can reach without a helper program; there
// mep.open_file_dialog uses mep's own directory picker instead.

#include <string>

namespace mep::file_dialog {

/** @brief Whether this platform has a native dialog Open() can show. */
bool Available();

/**
 * @brief Shows a modal "open file" dialog.
 * @param start_dir Directory it opens in ("" for the platform's default).
 * @return The chosen file's absolute path, or "" when the user cancelled (or there is no dialog).
 */
std::string Open(const std::string &start_dir);

}  // namespace mep::file_dialog

#endif  // MEP_FILE_DIALOG_H

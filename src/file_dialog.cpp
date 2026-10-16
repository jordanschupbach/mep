// file_dialog.h everywhere but macOS (file_dialog_macos.mm): the Windows
// common dialog, and no dialog at all on Linux and the web build.

#include "file_dialog.h"

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>

#include <vector>
#endif

namespace mep::file_dialog {

#if defined(_WIN32)

namespace {

std::wstring Widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

bool Available() { return true; }

std::string Open(const std::string &start_dir) {
    std::vector<wchar_t> buf(32768, L'\0');
    const std::wstring dir = Widen(start_dir);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = L"Open File";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return "";
    return Narrow(buf.data());
}

#else

bool Available() { return false; }

std::string Open(const std::string &) { return ""; }

#endif

}  // namespace mep::file_dialog

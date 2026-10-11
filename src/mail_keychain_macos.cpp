// The mail client's macOS Keychain access (mail_net.cpp, Account::secret ==
// "keychain"), through the Security framework rather than the `security`
// command. That difference is the point: an item created by a program
// trusts *that program*. Made through `security`, any process could read
// the password back silently by running `security find-generic-password`;
// made by mep itself, macOS asks you before anything else -- `security`
// included -- may read it. (A rebuilt mep is a different program to macOS,
// so the first read after a rebuild asks once too: "Always Allow".)

#include "mail_keychain_macos.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <mutex>

namespace mail::keychain {

namespace {

// Marks items written by this code, so items the `security` command made
// (which trust that command) are recognised and re-created as mep's own.
constexpr char kMarker[] = "mep-v2";

std::mutex g_mutex;  // SecKeychainSetUserInteractionAllowed is process-wide

CFStringRef Str(const std::string &s) {
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(s.data()),
                                   static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false);
}

CFMutableDictionaryRef Query(const std::string &service, const std::string &account) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    CFStringRef svc = Str(service), acc = Str(account);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, svc);
    CFDictionarySetValue(q, kSecAttrAccount, acc);
    CFRelease(svc);
    CFRelease(acc);
    return q;
}

// Turns off (or back on) the "allow access?" dialogs for this process.
// Deprecated but still the one switch that covers the file-based login
// keychain's ACL prompts; held under g_mutex for the duration of a call.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
void AllowUi(bool allow) { SecKeychainSetUserInteractionAllowed(allow ? TRUE : FALSE); }
#pragma clang diagnostic pop

Status FromOSStatus(OSStatus st) {
    if (st == errSecSuccess) return Status::Ok;
    if (st == errSecItemNotFound) return Status::NotFound;
    // Not allowed for this program: reading reports errSecAuthFailed,
    // deleting someone else's item errSecInvalidOwnerEdit.
    if (st == errSecInteractionNotAllowed || st == errSecAuthFailed || st == errSecUserCanceled ||
        st == errSecInvalidOwnerEdit) {
        return Status::Denied;
    }
    return Status::Error;
}

}  // namespace

Status Read(const std::string &service, const std::string &account, bool allow_ui, std::string *secret, bool *ours) {
    std::lock_guard<std::mutex> lock(g_mutex);
    CFMutableDictionaryRef q = Query(service, account);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    AllowUi(allow_ui);
    CFTypeRef result = nullptr;
    const OSStatus st = SecItemCopyMatching(q, &result);
    AllowUi(true);
    CFRelease(q);
    if (st != errSecSuccess || !result) return FromOSStatus(st);
    *ours = false;
    if (CFGetTypeID(result) == CFDictionaryGetTypeID()) {
        auto dict = static_cast<CFDictionaryRef>(result);
        auto data = static_cast<CFDataRef>(CFDictionaryGetValue(dict, kSecValueData));
        if (data) {
            secret->assign(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), static_cast<size_t>(CFDataGetLength(data)));
        }
        auto comment = static_cast<CFStringRef>(CFDictionaryGetValue(dict, kSecAttrComment));
        if (comment) {
            char buf[32] = {0};
            if (CFStringGetCString(comment, buf, sizeof(buf), kCFStringEncodingUTF8)) *ours = std::string(buf) == kMarker;
        }
    }
    CFRelease(result);
    return Status::Ok;
}

Status Delete(const std::string &service, const std::string &account, bool allow_ui) {
    std::lock_guard<std::mutex> lock(g_mutex);
    CFMutableDictionaryRef q = Query(service, account);
    AllowUi(allow_ui);
    const OSStatus st = SecItemDelete(q);
    AllowUi(true);
    CFRelease(q);
    return FromOSStatus(st);
}

Status Add(const std::string &service, const std::string &account, const std::string &label, const std::string &secret,
           bool allow_ui) {
    std::lock_guard<std::mutex> lock(g_mutex);
    CFMutableDictionaryRef q = Query(service, account);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(secret.data()),
                                  static_cast<CFIndex>(secret.size()));
    CFStringRef lbl = Str(label), marker = Str(kMarker);
    CFDictionarySetValue(q, kSecValueData, data);
    CFDictionarySetValue(q, kSecAttrLabel, lbl);
    CFDictionarySetValue(q, kSecAttrComment, marker);
    // No explicit access object: the default trusts exactly the program
    // adding the item -- this mep -- and no other.
    AllowUi(allow_ui);
    const OSStatus st = SecItemAdd(q, nullptr);
    AllowUi(true);
    CFRelease(data);
    CFRelease(lbl);
    CFRelease(marker);
    CFRelease(q);
    return FromOSStatus(st);
}

}  // namespace mail::keychain

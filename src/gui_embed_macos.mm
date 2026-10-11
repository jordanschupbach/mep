// The macOS implementor of gui_embed.h: another program's window, kept
// floating exactly over the block's area of mep's own window.
//
// macOS has no reparenting: a window belongs to the process that made it,
// and no other process can draw it inside one of its own. What it does
// allow -- with the Accessibility permission -- is moving and resizing
// another program's window, and ordering one's own window directly
// beneath any other window on the screen. So the program's window stays a
// top-level window of its own, and this backend:
//   1. Finds it (MacBackend::Adopt): among the windows on screen
//      (CGWindowList), the ordinary one owned by the program or one of its
//      children -- the largest, if it has several, and never a dialog. Its
//      Accessibility element is the app's window whose frame matches.
//   2. Places it (MacWindow::Place): sets its position and size through
//      Accessibility to the part of the block on screen, in screen
//      coordinates. Nothing can clip another program's window, so where
//      the pane's edge would cut it off the window is *shrunk* to the
//      visible part instead. Its title bar stays: a program's window
//      keeps its frame here (there is no frameless client to pull out of
//      it, as on X11).
//   3. Keeps it over mep (MacWindow::EnsureAbove): mep's window is ordered
//      directly beneath it -- NSWindow's orderWindow:relativeTo: takes any
//      window's number, not only one's own. Activating mep brings mep's
//      windows to the front, over it, so the order is checked every frame
//      and mended. When other apps' windows have come between (the user
//      was in another app and came back), hiding and unhiding the
//      program first lifts its windows to just beneath mep's; ordering
//      mep's beneath the program's straight away would sink it under
//      those other windows too.
//   4. Hides it (MacWindow::Hide) by hiding its program, the one way to
//      take another app's window off the screen without minimising it to
//      the Dock.
//   5. Focus: there is no shield, so the pointer always reaches the
//      program (Backend::PointerThroughOnly); a click into it makes its
//      app the active one, which is what "it has the keyboard" means on
//      macOS. Focus(true) activates its app, Focus(false) activates mep
//      again. While it has the keyboard a global key monitor (Accessibility
//      again) watches for Ctrl-\, the one key that still reaches mep.
//   6. Pictures (MacWindow::Capture): ScreenCaptureKit, macOS 14 and
//      later (older ones leave no picture). Needs the Screen Recording
//      permission, asked for once; without it, too, the program leaves
//      no picture behind when it closes.
//
// Needs the Accessibility permission for mep -- or, for a mep started
// from a terminal, for that terminal: macOS attributes a command-line
// program's requests to the app that launched it.

#include "gui_embed.h"

#if defined(MEP_GUI_EMBED_MACOS)

// activateIgnoringOtherApps: is deprecated but is the call that still
// takes the keyboard back from another app from a plain (non-bundled)
// program; yieldActivationToApplication: is used alongside where it
// exists.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#import <ApplicationServices/ApplicationServices.h>
#import <Cocoa/Cocoa.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unistd.h>

#include "gfx/native_window_handle.h"

namespace mep::gui_embed {
namespace {

double Now() { return [[NSProcessInfo processInfo] systemUptime]; }

// MEP_GUI_EMBED_DEBUG=1 in the environment: what this backend does with a
// window, on stderr.
bool Debug() {
    static const bool on = std::getenv("MEP_GUI_EMBED_DEBUG") != nullptr;
    return on;
}
void Log(const char *what, CGWindowID wid, long a = 0, long b = 0) {
    if (Debug()) std::fprintf(stderr, "gui_embed[macos] window %u: %s %ld %ld\n", wid, what, a, b);
}

// --- CGWindowList ------------------------------------------------------------

struct WindowInfo {
    CGWindowID id = 0;
    pid_t pid = 0;
    int layer = 0;
    CGRect bounds = CGRectZero;  // screen coordinates, origin top-left
};

template <typename T>
bool DictNumber(CFDictionaryRef d, CFStringRef key, CFNumberType type, T *out) {
    const auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(d, key));
    return n != nullptr && CFGetTypeID(n) == CFNumberGetTypeID() && CFNumberGetValue(n, type, out);
}

// The windows `opts` selects, front to back.
std::vector<WindowInfo> ListWindows(CGWindowListOption opts, CGWindowID relative_to) {
    std::vector<WindowInfo> out;
    CFArrayRef list = CGWindowListCopyWindowInfo(opts, relative_to);
    if (!list) return out;
    for (CFIndex k = 0; k < CFArrayGetCount(list); ++k) {
        const auto d = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, k));
        WindowInfo w;
        int64_t id = 0, pid = 0, layer = 0;
        if (!DictNumber(d, kCGWindowNumber, kCFNumberSInt64Type, &id) || !DictNumber(d, kCGWindowOwnerPID, kCFNumberSInt64Type, &pid)) continue;
        DictNumber(d, kCGWindowLayer, kCFNumberSInt64Type, &layer);
        const auto bounds = static_cast<CFDictionaryRef>(CFDictionaryGetValue(d, kCGWindowBounds));
        if (!bounds || !CGRectMakeWithDictionaryRepresentation(bounds, &w.bounds)) continue;
        w.id = static_cast<CGWindowID>(id);
        w.pid = static_cast<pid_t>(pid);
        w.layer = static_cast<int>(layer);
        out.push_back(w);
    }
    CFRelease(list);
    return out;
}

// Whether the window server still has the window (on screen or not).
bool WindowExists(CGWindowID id) {
    const void *ids[] = {reinterpret_cast<const void *>(static_cast<uintptr_t>(id))};
    CFArrayRef arr = CFArrayCreate(nullptr, ids, 1, nullptr);
    if (!arr) return false;
    CFArrayRef desc = CGWindowListCreateDescriptionFromArray(arr);
    const bool exists = desc != nullptr && CFArrayGetCount(desc) > 0;
    if (desc) CFRelease(desc);
    CFRelease(arr);
    return exists;
}

// --- Accessibility -----------------------------------------------------------

template <typename T>
bool AXGetValue(AXUIElementRef e, CFStringRef attr, AXValueType type, T *out) {
    CFTypeRef v = nullptr;
    if (AXUIElementCopyAttributeValue(e, attr, &v) != kAXErrorSuccess || !v) return false;
    bool ok = false;
    if (CFGetTypeID(v) == AXValueGetTypeID()) {
        const auto value = static_cast<AXValueRef>(v);
        ok = AXValueGetType(value) == type && AXValueGetValue(value, type, out);
    }
    CFRelease(v);
    return ok;
}

bool AXGetString(AXUIElementRef e, CFStringRef attr, std::string *out) {
    CFTypeRef v = nullptr;
    if (AXUIElementCopyAttributeValue(e, attr, &v) != kAXErrorSuccess || !v) return false;
    bool ok = false;
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        const auto s = static_cast<CFStringRef>(v);
        const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
        std::string buf(static_cast<size_t>(max), '\0');
        if (CFStringGetCString(s, buf.data(), max, kCFStringEncodingUTF8)) {
            buf.resize(std::strlen(buf.c_str()));
            *out = std::move(buf);
            ok = true;
        }
    }
    CFRelease(v);
    return ok;
}

bool AXGetFrame(AXUIElementRef win, CGRect *frame) {
    CGPoint p;
    CGSize s;
    if (!AXGetValue(win, kAXPositionAttribute, kAXValueTypeCGPoint, &p) || !AXGetValue(win, kAXSizeAttribute, kAXValueTypeCGSize, &s)) return false;
    *frame = CGRectMake(p.x, p.y, s.width, s.height);
    return true;
}

bool AXSetFrame(AXUIElementRef win, CGRect frame) {
    CGPoint p = frame.origin;
    CGSize s = frame.size;
    AXValueRef pv = AXValueCreate(kAXValueTypeCGPoint, &p);
    AXValueRef sv = AXValueCreate(kAXValueTypeCGSize, &s);
    bool ok = pv && sv;
    // Position first, then size, then the position again: a window near
    // the screen's edge is pushed back by a size it cannot have there,
    // and some programs anchor a resize at their bottom-left.
    if (ok) ok = AXUIElementSetAttributeValue(win, kAXPositionAttribute, pv) == kAXErrorSuccess;
    if (ok) ok = AXUIElementSetAttributeValue(win, kAXSizeAttribute, sv) == kAXErrorSuccess;
    if (ok) AXUIElementSetAttributeValue(win, kAXPositionAttribute, pv);
    if (pv) CFRelease(pv);
    if (sv) CFRelease(sv);
    return ok;
}

bool SameRect(const CGRect &a, const CGRect &b, double tolerance) {
    return std::fabs(a.origin.x - b.origin.x) <= tolerance && std::fabs(a.origin.y - b.origin.y) <= tolerance &&
           std::fabs(a.size.width - b.size.width) <= tolerance && std::fabs(a.size.height - b.size.height) <= tolerance;
}

// A rectangle of mep's window (its content view's coordinates: pixels,
// origin top-left -- the GL surface is 1x, so a pixel is a point) in the
// screen coordinates Accessibility and CGWindowList use (points, origin
// at the top-left of the main screen).
CGRect ScreenRectOf(NSWindow *host, const Rect &r) {
    const NSRect in = NSMakeRect(r.x, r.y, r.w, r.h);
    NSView *view = host.contentView;
    const NSRect in_window = view ? [view convertRect:in toView:nil] : in;
    const NSRect on_screen = [host convertRectToScreen:in_window];
    const CGFloat main_height = [NSScreen screens].firstObject.frame.size.height;
    return CGRectMake(on_screen.origin.x, main_height - (on_screen.origin.y + on_screen.size.height), on_screen.size.width,
                      on_screen.size.height);
}

// RGBA pixels of a CGImage, at its own size.
Snapshot SnapshotOf(CGImageRef image) {
    Snapshot s;
    if (!image) return s;
    const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
    if (w == 0 || h == 0) return s;
    std::vector<unsigned char> rgba(w * h * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, cs,
                                             static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big));
    if (ctx) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), image);
        CGContextRelease(ctx);
        s.width = static_cast<int>(w);
        s.height = static_cast<int>(h);
        s.rgba = std::move(rgba);
    }
    CGColorSpaceRelease(cs);
    return s;
}

class MacWindow;

class MacBackend final : public Backend {
public:
    explicit MacBackend(NSWindow *host) : host_(host), self_pid_(getpid()) {
        // The system's own prompt for the Accessibility permission, once,
        // the first time a program is to be embedded.
        NSDictionary *opts = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt : @YES};
        AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts);
    }
    ~MacBackend() override {
        if (key_monitor_) [NSEvent removeMonitor:key_monitor_];
    }

    std::string Name() const override { return "macos"; }
    bool Usable(std::string *why) const override {
        if (AXIsProcessTrusted()) return true;
        if (why)
            *why = "macOS lets mep move another program's window only once it may control the computer: allow it under "
                   "System Settings > Privacy & Security > Accessibility (mep, or the terminal it was started from), then "
                   "start mep again";
        return false;
    }
    std::vector<std::pair<std::string, std::string>> ChildEnv() const override { return {}; }
    bool PointerThroughOnly() const override { return true; }
    void NoteExistingWindows() override {
        known_.clear();
        for (const WindowInfo &w : ListWindows(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID))
            known_.insert(w.id);
    }
    std::unique_ptr<EmbeddedWindow> Adopt(const std::vector<int> &pids, bool allow_unowned) override;
    std::unique_ptr<EmbeddedWindow> AdoptHandoff(const std::vector<std::string> &executables, const std::vector<int> &own_pids) override;
    std::string ResolveApplication(const std::string &name) const override;
    void Pump() override {}
    void Flush() override {}

    // --- for MacWindow --------------------------------------------------
    NSWindow *host() const { return host_; }
    // The window that is to hear Ctrl-\ pressed while its program has the
    // keyboard (null: none has it). A global key monitor sees keys other
    // apps get -- the Accessibility permission again.
    void WatchReleaseKey(MacWindow *w);
    void Forget(MacWindow *w) {
        if (release_target_ == w) WatchReleaseKey(nullptr);
    }
    // Whether windows' pictures may be taken: the Screen Recording
    // permission, asked for (the system's prompt) the first time it is
    // missing. Granted, it takes effect when mep is next started.
    bool ScreenCaptureAllowed() {
        if (CGPreflightScreenCaptureAccess()) return true;
        if (!capture_asked_) {
            capture_asked_ = true;
            CGRequestScreenCaptureAccess();
        }
        return false;
    }

private:
    // The largest ordinary on-screen window `wanted` accepts that is not a
    // dialog, as an Accessibility element. `shared`: see MacWindow.
    std::unique_ptr<EmbeddedWindow> AdoptWhere(const std::function<bool(const WindowInfo &)> &wanted, bool shared);

    NSWindow *host_;
    pid_t self_pid_;
    std::set<CGWindowID> known_;
    id key_monitor_ = nil;
    MacWindow *release_target_ = nullptr;
    bool capture_asked_ = false;
};

// What a capture in flight writes into: shared with its completion block,
// so one that lands after the window is gone writes into nothing that
// matters.
struct CaptureState {
    std::mutex mutex;
    Snapshot latest;
    bool inflight = false;
    id sc_window = nil;  // an SCWindow (typed loosely: the class is newer than some deployment targets)
    CGRect sc_frame = CGRectZero;  // the frame sc_window was fetched for
};

class MacWindow final : public EmbeddedWindow {
public:
    // `shared`: the window belongs to a program that has other windows the
    // user is working in (a handed-off one, MacBackend::AdoptHandoff), so
    // nothing here may hide or unhide the program as a whole -- hiding is
    // putting mep's window over this one instead.
    MacWindow(MacBackend &be, const WindowInfo &info, AXUIElementRef ax_app, AXUIElementRef ax_win, bool shared)
        : be_(be), pid_(info.pid), wid_(info.id), ax_app_(ax_app), ax_win_(ax_win), shared_(shared),
          capture_(std::make_shared<CaptureState>()) {
        app_ = [NSRunningApplication runningApplicationWithProcessIdentifier:pid_];
        adopted_at_ = Now();
        // A program that brought itself to the front on starting (Tk does)
        // is put back behind the document: adopted, the keyboard stays
        // with mep until asked for, as it does under the X11 shield.
        Log("adopted; its program is in front:", wid_, IsFront());
        if (IsFront()) [NSApp activateIgnoringOtherApps:YES];
        // Off the screen until the document places it (as a window just
        // reparented into X11's unmapped container is): wherever its
        // program first opened it is not where it belongs.
        if (shared_) {
            [be_.host() orderWindow:NSWindowAbove relativeTo:static_cast<NSInteger>(wid_)];
        } else if (app_) {
            [app_ hide];
            hidden_ = true;
        }
    }
    ~MacWindow() override {
        be_.Forget(this);
        if (!dead_ && !shared_) {
            if (focused_ && IsFront()) [NSApp activateIgnoringOtherApps:YES];
            // Still running, and hidden by this backend (its program is
            // being stopped, or has lost it): back on the screen, so a
            // program that ignores the stop is not left invisible.
            if (hidden_ && app_) [app_ unhide];
        }
        CFRelease(ax_win_);
        CFRelease(ax_app_);
    }

    void MarkReleaseRequest() { release_requested_ = true; }

    bool Alive() override {
        if (dead_) return false;
        const double t = Now();
        if (t - last_alive_check_ < 0.1) return true;
        last_alive_check_ = t;
        if ((app_ && app_.terminated) || !WindowExists(wid_)) dead_ = true;
        return !dead_;
    }

    void Place(const Rect &full, const Rect &clip) override {
        if (dead_) return;
        // The part of the window on screen: what it is shrunk to.
        Rect c;
        c.x = std::max(full.x, clip.x);
        c.y = std::max(full.y, clip.y);
        c.w = std::min(full.x + full.w, clip.x + clip.w) - c.x;
        c.h = std::min(full.y + full.h, clip.y + clip.h) - c.y;
        if (c.Empty()) {
            Hide();
            return;
        }
        if (hidden_ && app_) {
            Log("unhide to place", wid_);
            [app_ unhide];
            hidden_ = false;
        }
        shown_ = true;
        clipped_ = c != full;
        const CGRect want = ScreenRectOf(be_.host(), c);
        const double t = Now();
        bool set = !SameRect(want, frame_, 0.5);
        if (!set && t - last_frame_check_ >= 0.5) {
            // Moved or resized by the program itself (or dragged by its
            // title bar): put back.
            last_frame_check_ = t;
            CGRect now_frame;
            set = AXGetFrame(ax_win_, &now_frame) && !SameRect(now_frame, want, 1.0);
        }
        if (set) {
            AXSetFrame(ax_win_, want);
            frame_ = want;
            last_frame_check_ = t;
        }
        EnsureAbove();
    }

    void Hide() override {
        if (!shown_) return;
        shown_ = false;
        if (shared_) {
            if (focused_ && IsFront()) [NSApp activateIgnoringOtherApps:YES];
            [be_.host() orderWindow:NSWindowAbove relativeTo:static_cast<NSInteger>(wid_)];
            return;
        }
        if (app_ && !hidden_) {
            // Hiding the active app hands activation to whichever app is
            // next: mep takes it first, so it is mep.
            if (focused_ && IsFront()) [NSApp activateIgnoringOtherApps:YES];
            [app_ hide];
            hidden_ = true;
        }
    }

    void Focus(bool on) override {
        if (dead_) return;
        focused_ = on;
        if (on) {
            focus_asked_ = true;
            if (hidden_ && app_) {
                [app_ unhide];
                hidden_ = false;
            }
            be_.WatchReleaseKey(this);
            if (app_) {
                if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:app_];
                [app_ activateWithOptions:0];
            }
            AXUIElementPerformAction(ax_win_, kAXRaiseAction);  // its app's key window
        } else {
            be_.WatchReleaseKey(nullptr);
            // Back to mep -- unless the user has already put the focus
            // somewhere else entirely.
            if (IsFront()) [NSApp activateIgnoringOtherApps:YES];
        }
    }

    bool HasFocus() override {
        if (dead_ || !IsFront()) return false;
        // Its program came to the front by itself just after being
        // adopted (Tk does, on its first idle moment): not the user
        // clicking into it -- the document keeps the keyboard.
        if (!focus_asked_ && Now() - adopted_at_ < 1.5) {
            [NSApp activateIgnoringOtherApps:YES];
            return false;
        }
        return true;
    }

    bool TakeReleaseRequest() override {
        const bool r = release_requested_;
        release_requested_ = false;
        return r;
    }

    bool RequestClose() override {
        if (dead_) return false;
        CFTypeRef button = nullptr;
        if (AXUIElementCopyAttributeValue(ax_win_, kAXCloseButtonAttribute, &button) != kAXErrorSuccess || !button) return false;
        const bool ok = AXUIElementPerformAction(static_cast<AXUIElementRef>(button), kAXPressAction) == kAXErrorSuccess;
        CFRelease(button);
        return ok;
    }

    Snapshot Capture() override {
        Snapshot s;
        if (dead_ || !shown_ || !be_.ScreenCaptureAllowed()) return s;
        // Asynchronous: what comes back is the picture the next call
        // returns (one is taken every quarter second anyway). Before
        // macOS 14 there is no picture: CGWindowListCreateImage is gone
        // from the SDK and ScreenCaptureKit cannot take a single shot.
        if (@available(macOS 14.0, *)) {
            StartCapture();
            std::lock_guard<std::mutex> lock(capture_->mutex);
            s = capture_->latest;
        }
        return s;
    }

    std::string Title() override {
        std::string title;
        if (!dead_) AXGetString(ax_win_, kAXTitleAttribute, &title);
        return title;
    }

private:
    // Whether its program is the active (frontmost) one.
    bool IsFront() const {
        NSRunningApplication *front = [NSWorkspace sharedWorkspace].frontmostApplication;
        return front != nil && front.processIdentifier == pid_;
    }

    // mep's window directly beneath the program's (see the top comment).
    void EnsureAbove() {
        NSWindow *host = be_.host();
        const auto host_id = static_cast<CGWindowID>(host.windowNumber);
        enum { Above, DirectlyBeneath, Beneath, OffScreen } where = OffScreen;
        for (const WindowInfo &w : ListWindows(kCGWindowListOptionOnScreenAboveWindow, host_id))
            if (w.id == wid_) where = Above;
        if (where == OffScreen) {
            bool first = true;
            for (const WindowInfo &w : ListWindows(kCGWindowListOptionOnScreenBelowWindow, host_id)) {
                // (Its program's other windows, and anything not an
                // ordinary window, do not count as being in between.)
                if (w.layer != 0 || (w.pid == pid_ && w.id != wid_)) continue;
                if (w.id == wid_) {
                    where = first ? DirectlyBeneath : Beneath;
                    break;
                }
                first = false;
            }
        }
        if (where != last_where_) {
            Log("stacking: 0 above, 1 directly beneath, 2 beneath, 3 off screen", wid_, where, lifts_);
            last_where_ = where;
        }
        switch (where) {
            case Above: lifts_ = 0; return;
            // Just unhidden, not on the screen yet (unhiding is
            // asynchronous): ordering relative to it now would order
            // relative to nowhere. Next frame.
            case OffScreen: return;
            case DirectlyBeneath:
                [host orderWindow:NSWindowBelow relativeTo:static_cast<NSInteger>(wid_)];
                lifts_ = 0;
                return;
            case Beneath: break;
        }
        // Other apps' windows between (the user was in another app and
        // came back: mep's windows jumped over everything): ordering mep's
        // window beneath the program's now would sink it under those too.
        // The program's is lifted first -- hiding and unhiding it puts its
        // windows just beneath the active app's, with nothing visible
        // switching; should that not do, activating it and then mep again
        // does (a flash of its menu bar at most), but only while mep is
        // the active app, never to take the keyboard from another; and
        // failing all that, under those windows mep's window goes (a
        // click brings it back).
        const double t = Now();
        if (t - last_lift_ < 0.5) return;
        last_lift_ = t;
        Log("lift", wid_, lifts_, [NSApp isActive]);
        if (lifts_ < 2 && app_ && !shared_) {
            [app_ hide];
            [app_ unhide];
        } else if (lifts_ < 4 && app_ && [NSApp isActive]) {
            if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:app_];
            [app_ activateWithOptions:0];
            [NSApp activateIgnoringOtherApps:YES];
        } else {
            [host orderWindow:NSWindowBelow relativeTo:static_cast<NSInteger>(wid_)];
        }
        ++lifts_;
    }

    void StartCapture() API_AVAILABLE(macos(14.0)) {
        std::shared_ptr<CaptureState> cap = capture_;
        {
            std::lock_guard<std::mutex> lock(cap->mutex);
            if (cap->inflight) return;
            cap->inflight = true;
        }
        const CGRect frame = frame_;
        const bool clipped = clipped_;
        const CGWindowID wid = wid_;
        // The window as ScreenCaptureKit knows it, fetched again when its
        // frame has changed since (what it was fetched for is what the
        // picture is sized by).
        SCWindow *sc_window = nil;
        {
            std::lock_guard<std::mutex> lock(cap->mutex);
            if (cap->sc_window && SameRect(cap->sc_frame, frame, 0.5)) sc_window = static_cast<SCWindow *>(cap->sc_window);
        }
        auto shoot = [cap, frame, clipped](SCWindow *window) {
            SCContentFilter *filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
            SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
            // At the size mep laid it out in (points): the picture a 1x
            // surface draws pixel for pixel.
            config.width = static_cast<size_t>(std::max(1.0, frame.size.width));
            config.height = static_cast<size_t>(std::max(1.0, frame.size.height));
            config.captureResolution = SCCaptureResolutionNominal;
            config.showsCursor = NO;
            config.ignoreShadowsSingleWindow = YES;
            [SCScreenshotManager captureImageWithFilter:filter
                                          configuration:config
                                      completionHandler:^(CGImageRef image, NSError *error) {
                                        Snapshot s = error ? Snapshot{} : SnapshotOf(image);
                                        s.clipped = clipped;
                                        std::lock_guard<std::mutex> lock(cap->mutex);
                                        if (!s.Empty()) cap->latest = std::move(s);
                                        cap->inflight = false;
                                      }];
        };
        if (sc_window) {
            shoot(sc_window);
            return;
        }
        [SCShareableContent getShareableContentExcludingDesktopWindows:YES
                                                   onScreenWindowsOnly:YES
                                                     completionHandler:^(SCShareableContent *content, NSError *error) {
                                                       SCWindow *found = nil;
                                                       if (!error)
                                                           for (SCWindow *w in content.windows)
                                                               if (w.windowID == wid) found = w;
                                                       if (!found) {
                                                           std::lock_guard<std::mutex> lock(cap->mutex);
                                                           cap->inflight = false;
                                                           return;
                                                       }
                                                       {
                                                           std::lock_guard<std::mutex> lock(cap->mutex);
                                                           cap->sc_window = found;
                                                           cap->sc_frame = frame;
                                                       }
                                                       shoot(found);
                                                     }];
    }

    MacBackend &be_;
    pid_t pid_;
    CGWindowID wid_;
    AXUIElementRef ax_app_, ax_win_;
    bool shared_ = false;
    NSRunningApplication *app_ = nil;
    std::shared_ptr<CaptureState> capture_;
    CGRect frame_ = CGRectZero;  // where it was last put (screen coordinates)
    double adopted_at_ = 0.0, last_alive_check_ = -1.0, last_frame_check_ = -1.0, last_lift_ = -1.0;
    int lifts_ = 0;  // lifts tried since the window was last seen above mep's
    int last_where_ = -1;
    bool shown_ = false, hidden_ = false, focused_ = false, focus_asked_ = false, clipped_ = false;
    bool dead_ = false, release_requested_ = false;
};

void MacBackend::WatchReleaseKey(MacWindow *w) {
    release_target_ = w;
    if (w && !key_monitor_) {
        MacBackend *self = this;
        key_monitor_ = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                              handler:^(NSEvent *e) {
                                                                if (!(e.modifierFlags & NSEventModifierFlagControl)) return;
                                                                // kVK_ANSI_Backslash, or "\" wherever the layout has it.
                                                                const bool backslash = e.keyCode == 42 || [e.charactersIgnoringModifiers isEqualToString:@"\\"];
                                                                if (backslash && self->release_target_) self->release_target_->MarkReleaseRequest();
                                                              }];
    } else if (!w && key_monitor_) {
        [NSEvent removeMonitor:key_monitor_];
        key_monitor_ = nil;
    }
}

std::unique_ptr<EmbeddedWindow> MacBackend::Adopt(const std::vector<int> &pids, bool allow_unowned) {
    // (Every macOS window names its process: allow_unowned has nothing to
    // allow.)
    (void)allow_unowned;
    return AdoptWhere([&](const WindowInfo &w) { return std::find(pids.begin(), pids.end(), static_cast<int>(w.pid)) != pids.end(); },
                      false);
}

std::unique_ptr<EmbeddedWindow> MacBackend::AdoptHandoff(const std::vector<std::string> &executables, const std::vector<int> &own_pids) {
    if (executables.empty()) return nullptr;
    // (Looked up once per process per call: a handful of windows.)
    std::map<pid_t, bool> runs_it;
    return AdoptWhere(
        [&](const WindowInfo &w) {
            if (known_.count(w.id) || std::find(own_pids.begin(), own_pids.end(), static_cast<int>(w.pid)) != own_pids.end()) return false;
            auto it = runs_it.find(w.pid);
            if (it == runs_it.end()) {
                const std::string exe = ProcessExecutable(static_cast<int>(w.pid));
                it = runs_it.emplace(w.pid, std::find(executables.begin(), executables.end(), exe) != executables.end()).first;
            }
            return it->second;
        },
        true);
}

std::string MacBackend::ResolveApplication(const std::string &name) const {
    if (name.empty() || name.find('/') != std::string::npos) return "";
    @autoreleasepool {
        NSString *want = [[NSString stringWithUTF8String:name.c_str()] lowercaseString];
        if (![want hasSuffix:@".app"]) want = [want stringByAppendingString:@".app"];
        NSMutableArray<NSString *> *dirs = [NSMutableArray arrayWithArray:@[
            @"/Applications", @"/Applications/Utilities", @"/System/Applications", @"/System/Applications/Utilities",
            @"/Applications/Nix Apps"
        ]];
        [dirs addObject:[NSHomeDirectory() stringByAppendingPathComponent:@"Applications"]];
        [dirs addObject:[NSHomeDirectory() stringByAppendingPathComponent:@"Applications/Home Manager Apps"]];
        NSFileManager *fm = [NSFileManager defaultManager];
        for (NSString *dir in dirs) {
            for (NSString *entry in [fm contentsOfDirectoryAtPath:dir error:nil]) {
                if (![[entry lowercaseString] isEqualToString:want]) continue;
                NSBundle *bundle = [NSBundle bundleWithPath:[dir stringByAppendingPathComponent:entry]];
                NSString *exe = bundle.executablePath;
                if (exe.length) return [[exe stringByResolvingSymlinksInPath] UTF8String];
            }
        }
    }
    return "";
}

std::unique_ptr<EmbeddedWindow> MacBackend::AdoptWhere(const std::function<bool(const WindowInfo &)> &wanted, bool shared) {
    std::vector<WindowInfo> candidates;
    for (const WindowInfo &w : ListWindows(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID)) {
        if (w.layer != 0 || w.pid == self_pid_ || w.bounds.size.width < 2 || w.bounds.size.height < 2) continue;
        if (!wanted(w)) continue;
        candidates.push_back(w);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const WindowInfo &a, const WindowInfo &b) {
        return a.bounds.size.width * a.bounds.size.height > b.bounds.size.width * b.bounds.size.height;
    });
    for (const WindowInfo &cand : candidates) {
        // Its Accessibility element: the app's window with the same frame
        // -- and not a dialog, which is never a program's main window.
        AXUIElementRef app = AXUIElementCreateApplication(cand.pid);
        if (!app) continue;
        // A program busy (or hung) must not hold mep: every request to it
        // gives up quickly.
        AXUIElementSetMessagingTimeout(app, 0.5f);
        CFTypeRef windows = nullptr;
        AXUIElementRef found = nullptr;
        if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &windows) == kAXErrorSuccess && windows &&
            CFGetTypeID(windows) == CFArrayGetTypeID()) {
            const auto arr = static_cast<CFArrayRef>(windows);
            for (CFIndex k = 0; k < CFArrayGetCount(arr) && !found; ++k) {
                const auto win = static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(arr, k));
                CGRect frame;
                if (!AXGetFrame(win, &frame) || !SameRect(frame, cand.bounds, 2.0)) continue;
                std::string subrole;
                AXGetString(win, kAXSubroleAttribute, &subrole);
                if (subrole == "AXDialog" || subrole == "AXSystemDialog") continue;
                found = static_cast<AXUIElementRef>(CFRetain(win));
            }
        }
        if (windows) CFRelease(windows);
        if (found) {
            AXUIElementSetMessagingTimeout(found, 0.5f);
            return std::make_unique<MacWindow>(*this, cand, app, found, shared);
        }
        CFRelease(app);
    }
    return nullptr;
}

}  // namespace

std::unique_ptr<Backend> CreateMacOSBackend(void *native_window_handle, std::string *why) {
    const auto *handle = static_cast<const gfx::NativeWindowHandle *>(native_window_handle);
    NSWindow *host = handle ? (__bridge NSWindow *)handle->display : nil;
    if (!host) {
        if (why) *why = "mep's window is not a Cocoa window";
        return nullptr;
    }
    return std::make_unique<MacBackend>(host);
}

}  // namespace mep::gui_embed

#endif  // MEP_GUI_EMBED_MACOS

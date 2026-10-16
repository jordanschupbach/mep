// gui_embed.h's WebView on macOS: a WKWebView -- the system's own WebKit,
// the engine Safari runs on -- inside mep's window. What :Browser shows.
//
// Unlike gui_embed_macos.mm (another program's window, floated over the
// pane through the Accessibility API), this page is a view of mep's own:
// a subview of mep's GL content view, laid over the pane's text area. No
// permission is involved, the pane's edge clips it for real, and it moves
// and hides with the pane like anything else mep draws. The view is
//
//   container_  (clips to the pane's area; hidden when nothing placed it)
//     page_     (the whole area: the address bar over the web view)
//       bar_      back / forward / reload buttons and the address field
//       web_      the WKWebView
//
// The keyboard: a click into the page (or EmbeddedApp::Focus) makes the
// web view the window's first responder, so AppKit hands it the keys
// directly -- mep's GL view sees none of them. A local key monitor (no
// permission: it only sees mep's own events) keeps back what is still
// mep's: Ctrl-\ gives the keyboard back, and mep's mod1 chords are kept
// from the page and handed to mep (TakeReservedChord), as the
// Accessibility backend's event tap does for a Mac app. It also gives the
// page the browser's usual Command keys, since mep's menu bar has no Edit
// menu to route copy/paste through.
//
// The theme (SetTheme): the address bar is drawn in mep's colours, the
// page's prefers-color-scheme follows mep's theme, and web_theme_script.h,
// run in every page in a script world of its own, recolours the page.

#include "gui_embed.h"

#if defined(MEP_GUI_EMBED_MACOS)

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "gfx/native_window_handle.h"
#include "web_theme_script.h"

namespace mep::gui_embed {
class MacWebView;
}

// Flipped, so frames inside it are in mep's own top-left-origin pixels.
@interface MepWebContainer : NSView
@end

@implementation MepWebContainer
- (BOOL)isFlipped {
    return YES;
}
@end

// The web view's and the address bar's delegate: forwards to the C++ side.
@interface MepWebDelegate : NSObject <WKNavigationDelegate, WKUIDelegate, NSTextFieldDelegate> {
  @public
    mep::gui_embed::MacWebView *owner;
}
@end

namespace mep::gui_embed {

namespace {

constexpr CGFloat kBarHeight = 30.0;

// `s`, or "" for nil.
NSString *OrEmpty(NSString *s) { return s ? s : @""; }

NSString *NS(const std::string &s) { return OrEmpty([NSString stringWithUTF8String:s.c_str()]); }

std::string Str(NSString *s) { return s ? std::string(s.UTF8String ? s.UTF8String : "") : std::string(); }

std::string Trim(const std::string &s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// The same names the Accessibility backend's event tap gives a key code
// (Backend::SetReservedChords's terms).
std::string KeyName(unsigned short code) {
    static const std::map<unsigned short, std::string> kNames = {
        {0, "a"},  {11, "b"}, {8, "c"},  {2, "d"},  {14, "e"}, {3, "f"},  {5, "g"},  {4, "h"},  {34, "i"},
        {38, "j"}, {40, "k"}, {37, "l"}, {46, "m"}, {45, "n"}, {31, "o"}, {35, "p"}, {12, "q"}, {15, "r"},
        {1, "s"},  {17, "t"}, {32, "u"}, {9, "v"},  {13, "w"}, {7, "x"},  {16, "y"}, {6, "z"},  {29, "0"},
        {18, "1"}, {19, "2"}, {20, "3"}, {21, "4"}, {23, "5"}, {22, "6"}, {26, "7"}, {28, "8"}, {25, "9"},
        {48, "Tab"}, {36, "CR"}, {76, "CR"}};
    auto it = kNames.find(code);
    return it == kNames.end() ? std::string() : it->second;
}

bool IsAllDigits(const std::string &s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); }

// What the address bar (and WebView::Load) makes of what was typed.
NSURL *UrlFor(const std::string &typed) {
    std::string t = Trim(typed);
    if (t.empty()) return [NSURL URLWithString:@"about:blank"];
    if (t[0] == '~') {
        const char *home = std::getenv("HOME");
        if (home) t = std::string(home) + t.substr(1);
    }
    if (t[0] == '/') return [NSURL fileURLWithPath:NS(t)];
    const std::string lower = [NS(t) lowercaseString].UTF8String;
    // A scheme of its own: as it is.
    if (lower.find("://") != std::string::npos || lower.rfind("about:", 0) == 0 || lower.rfind("data:", 0) == 0 ||
        lower.rfind("file:", 0) == 0 || lower.rfind("javascript:", 0) == 0) {
        if (NSURL *url = [NSURL URLWithString:NS(t)]) return url;
    }
    const bool has_space = t.find_first_of(" \t") != std::string::npos;
    if (!has_space) {
        // ":8000/app" and "localhost:8000": a server on this machine.
        if (t[0] == ':') return [NSURL URLWithString:NS("http://localhost" + t)];
        const std::string host = lower.substr(0, lower.find_first_of("/?#"));
        const std::string name = host.substr(0, host.find(':'));
        const std::string port = host.find(':') == std::string::npos ? "" : host.substr(host.find(':') + 1);
        if (name == "localhost" || name.rfind("127.", 0) == 0 || name == "0.0.0.0")
            return [NSURL URLWithString:NS("http://" + t)];
        // A file under mep's working directory ("index.html" is one before
        // it is a host name).
        std::error_code ec;
        if (std::filesystem::exists(t, ec)) return [NSURL fileURLWithPath:NS(std::filesystem::absolute(t, ec).string())];
        // "example.com", "news.ycombinator.com/item?id=1": a host name.
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0 && dot + 2 <= name.size() && (port.empty() || IsAllDigits(port))) {
            if (NSURL *url = [NSURL URLWithString:NS("https://" + t)]) return url;
        }
    }
    // Anything else is a search.
    NSString *q = [NS(t) stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLQueryAllowedCharacterSet]];
    q = [q stringByReplacingOccurrencesOfString:@"&" withString:@"%26"];
    q = [q stringByReplacingOccurrencesOfString:@"+" withString:@"%2B"];
    return [NSURL URLWithString:[@"https://duckduckgo.com/?q=" stringByAppendingString:OrEmpty(q)]];
}

NSColor *Color(WebView::Rgb c) { return [NSColor colorWithSRGBRed:c.r / 255.0 green:c.g / 255.0 blue:c.b / 255.0 alpha:1.0]; }

std::string Hex(WebView::Rgb c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

// web_theme_script.h's MEP_THEME.
std::string ThemeJson(const WebView::Theme &t) {
    auto q = [](WebView::Rgb c) { return "\"" + Hex(c) + "\""; };
    return std::string("{\"recolor\":") + (t.recolor ? "true" : "false") + ",\"dark\":" + (t.dark ? "true" : "false") +
           ",\"bg\":" + q(t.bg) + ",\"fg\":" + q(t.fg) + ",\"dim\":" + q(t.dim) + ",\"selection\":" + q(t.selection) +
           ",\"border\":" + q(t.border) + ",\"pal\":{\"red\":" + q(t.red) + ",\"green\":" + q(t.green) +
           ",\"yellow\":" + q(t.yellow) + ",\"blue\":" + q(t.blue) + ",\"purple\":" + q(t.purple) + ",\"cyan\":" + q(t.cyan) +
           ",\"orange\":" + q(t.orange) + "}}";
}

NSButton *BarButton(NSString *symbol, NSString *tip, id target, SEL action) {
    NSImage *image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:tip];
    NSButton *b = [NSButton buttonWithImage:(image ? image : [[NSImage alloc] init]) target:target action:action];
    b.bordered = NO;
    b.toolTip = tip;
    b.refusesFirstResponder = YES;
    return b;
}

}  // namespace

class MacWebView final : public WebView {
public:
    MacWebView(NSWindow *host, std::function<bool(const std::string &, unsigned)> reserved) : host_(host), reserved_(std::move(reserved)) {
        delegate_ = [[MepWebDelegate alloc] init];
        delegate_->owner = this;

        container_ = [[MepWebContainer alloc] initWithFrame:NSZeroRect];
        container_.clipsToBounds = YES;
        container_.hidden = YES;
        page_ = [[MepWebContainer alloc] initWithFrame:NSZeroRect];
        [container_ addSubview:page_];

        bar_ = [[NSView alloc] initWithFrame:NSZeroRect];
        bar_.wantsLayer = YES;
        bar_.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
        back_ = BarButton(@"chevron.left", @"Back (Cmd-[)", delegate_, @selector(goBack:));
        forward_ = BarButton(@"chevron.right", @"Forward (Cmd-])", delegate_, @selector(goForward:));
        reload_ = BarButton(@"arrow.clockwise", @"Reload (Cmd-R)", delegate_, @selector(reloadOrStop:));
        address_ = [[NSTextField alloc] initWithFrame:NSZeroRect];
        address_.placeholderString = @"Search or enter address";
        address_.bezelStyle = NSTextFieldRoundedBezel;
        address_.font = [NSFont systemFontOfSize:12];
        address_.lineBreakMode = NSLineBreakByTruncatingTail;
        address_.usesSingleLineMode = YES;
        address_.target = delegate_;
        address_.action = @selector(addressEntered:);
        address_.delegate = delegate_;
        for (NSView *v in @[ back_, forward_, reload_, address_ ]) [bar_ addSubview:v];
        [page_ addSubview:bar_];

        WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
        // Without a "Safari" in the user agent, many sites serve a cut-down
        // page to what they take for an embedded browser.
        config.applicationNameForUserAgent = @"Version/18.0 Safari/605.1.15";
        config.preferences.javaScriptCanOpenWindowsAutomatically = YES;
        @try {
            // "Inspect Element" in the page's context menu.
            [config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
        } @catch (NSException *) {
        }
        web_ = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:config];
        web_.navigationDelegate = delegate_;
        web_.UIDelegate = delegate_;
        web_.allowsBackForwardNavigationGestures = YES;
        web_.allowsMagnification = YES;
        if (@available(macOS 13.3, *)) web_.inspectable = YES;
        [page_ addSubview:web_];

        [host_.contentView addSubview:container_];
        UpdateButtons();

        __unsafe_unretained MepWebDelegate *weak_delegate = delegate_;
        monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                         handler:^NSEvent *(NSEvent *e) {
                                                           MacWebView *self = weak_delegate ? weak_delegate->owner : nullptr;
                                                           return self ? self->OnKey(e) : e;
                                                         }];
    }

    ~MacWebView() override { TearDown(); }

    // --- EmbeddedWindow ---------------------------------------------------

    bool Alive() override { return alive_; }

    void Place(const Rect &full, const Rect &clip) override {
        if (!alive_) return;
        Rect box;
        box.x = std::max(full.x, clip.x);
        box.y = std::max(full.y, clip.y);
        box.w = std::min(full.x + full.w, clip.x + clip.w) - box.x;
        box.h = std::min(full.y + full.h, clip.y + clip.h) - box.y;
        if (box.Empty()) {
            Hide();
            return;
        }
        if (full != full_ || box != box_) {
            full_ = full;
            box_ = box;
            container_.frame = NSMakeRect(box.x, box.y, box.w, box.h);
            page_.frame = NSMakeRect(full.x - box.x, full.y - box.y, full.w, full.h);
            Layout();
        }
        if (container_.hidden) container_.hidden = NO;
        // What was asked for before the page was ever on screen.
        if (pending_ == Pending::Address) {
            [host_ makeFirstResponder:address_];
            [address_ selectText:nil];
        } else if (pending_ == Pending::Page) {
            [host_ makeFirstResponder:web_];
        }
        pending_ = Pending::None;
    }

    void Hide() override {
        if (!alive_ || container_.hidden) return;
        if (Inside(host_.firstResponder)) [host_ makeFirstResponder:host_.contentView];
        container_.hidden = YES;
    }

    void Focus(bool on) override {
        if (!alive_) return;
        if (on) {
            if (Inside(host_.firstResponder)) return;
            if (container_.hidden) pending_ = Pending::Page;
            else [host_ makeFirstResponder:web_];
        } else {
            pending_ = Pending::None;
            if (Inside(host_.firstResponder)) [host_ makeFirstResponder:host_.contentView];
        }
    }

    bool HasFocus() override { return alive_ && (pending_ != Pending::None || Inside(host_.firstResponder)); }

    bool TakeReleaseRequest() override {
        const bool r = release_request_;
        release_request_ = false;
        return r;
    }

    bool TakeReservedChord(std::string *key, unsigned *mods) override {
        if (chord_key_.empty()) return false;
        *key = chord_key_;
        *mods = chord_mods_;
        chord_key_.clear();
        return true;
    }

    bool RequestClose() override {
        TearDown();
        return true;
    }

    // The page's picture, for a mepml results=web run (an :exec pane keeps
    // none, so is never asked). WebKit draws one asynchronously: this hands
    // back the last one finished and asks for the next. The page is drawn
    // whole even where the pane's edge cuts the view off, at 1x, as mep
    // lays everything out.
    Snapshot Capture() override {
        if (!alive_) return snap_->latest;
        const CGSize size = web_.bounds.size;
        // (WebKit may never answer for a view that has no page yet -- a web
        // app's, until its server says where it listens -- so nothing is
        // asked before there is one, and an answer that never came is
        // given up on.)
        const double now = [NSProcessInfo processInfo].systemUptime;
        if (snap_->pending && now - snap_->asked_at > 3.0) snap_->pending = false;
        if (!snap_->pending && !container_.hidden && web_.URL && !web_.loading && size.width >= 1 && size.height >= 1) {
            snap_->pending = true;
            snap_->asked_at = now;
            WKSnapshotConfiguration *cfg = [[WKSnapshotConfiguration alloc] init];
            cfg.afterScreenUpdates = NO;
            std::shared_ptr<SnapState> st = snap_;
            const size_t w = static_cast<size_t>(size.width), h = static_cast<size_t>(size.height);
            [web_ takeSnapshotWithConfiguration:cfg
                              completionHandler:^(NSImage *image, NSError *) {
                                st->pending = false;
                                CGImageRef cg = image ? [image CGImageForProposedRect:nullptr context:nil hints:nil] : nullptr;
                                if (!cg) return;
                                Snapshot out;
                                out.width = static_cast<int>(w);
                                out.height = static_cast<int>(h);
                                out.rgba.assign(w * h * 4, 0);
                                CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                                CGContextRef ctx = CGBitmapContextCreate(
                                    out.rgba.data(), w, h, 8, w * 4, cs,
                                    static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
                                CGColorSpaceRelease(cs);
                                if (!ctx) return;
                                CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
                                CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), cg);
                                CGContextRelease(ctx);
                                st->latest = std::move(out);
                              }];
        }
        return snap_->latest;
    }

    std::string Title() override {
        if (!alive_) return "";
        // (Asked every frame. A single-page app moving through its own
        // history -- pushState -- commits no navigation: this is where the
        // address bar catches up with it.)
        if (!Editing() && ![address_.stringValue isEqualToString:OrEmpty(web_.URL.absoluteString)]) NavigationChanged();
        std::string t = Trim(Str(web_.title));
        if (t.empty()) t = Str(web_.URL.host);
        return t;
    }

    // --- WebView ------------------------------------------------------------

    void Load(const std::string &target) override {
        if (!alive_) return;
        NSURL *url = UrlFor(target);
        if (url.isFileURL) {
            NSURL *dir = url.hasDirectoryPath ? url : url.URLByDeletingLastPathComponent;
            [web_ loadFileURL:url allowingReadAccessToURL:dir];
        } else {
            [web_ loadRequest:[NSURLRequest requestWithURL:url]];
        }
        address_.stringValue = OrEmpty(url.absoluteString);
        UpdateButtons();
    }

    void Back() override {
        if (alive_ && web_.canGoBack) [web_ goBack];
    }

    void Forward() override {
        if (alive_ && web_.canGoForward) [web_ goForward];
    }

    void Reload() override {
        if (!alive_) return;
        if (web_.loading) [web_ stopLoading];
        else if (web_.URL) [web_ reload];
        UpdateButtons();
    }

    void EditAddress() override {
        if (!alive_) return;
        if (container_.hidden) {
            pending_ = Pending::Address;
            return;
        }
        [host_ makeFirstResponder:address_];
        [address_ selectText:nil];
    }

    std::string Url() override { return alive_ ? Str(web_.URL.absoluteString) : ""; }

    void SetTheme(const Theme &t) override {
        if (!alive_) return;
        NSAppearance *look = [NSAppearance appearanceNamed:t.dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
        // The address bar: always mep's.
        bar_.appearance = look;
        bar_.layer.backgroundColor = Color(t.bar).CGColor;
        address_.bezeled = NO;
        address_.drawsBackground = YES;
        address_.backgroundColor = Color(t.bg);
        address_.textColor = Color(t.fg);
        address_.wantsLayer = YES;
        address_.layer.cornerRadius = 5;
        address_.layer.borderWidth = 1;
        address_.layer.borderColor = Color(t.border).CGColor;
        address_.placeholderAttributedString = [[NSAttributedString alloc]
            initWithString:@"Search or enter address"
                attributes:@{NSForegroundColorAttributeName : Color(t.dim), NSFontAttributeName : address_.font}];
        for (NSButton *b in @[ back_, forward_, reload_ ]) b.contentTintColor = Color(t.fg);
        // The page: prefers-color-scheme says what mep's theme is (or,
        // recolouring off, what the system's is), and the script does the
        // rest -- in pages to come, and in this one.
        web_.appearance = t.recolor ? look : nil;
        web_.underPageBackgroundColor = t.recolor ? Color(t.bg) : nil;
        NSString *source = NS("{ const MEP_THEME = " + ThemeJson(t) + ";\n" + kWebThemeScript + "}\n");
        WKUserContentController *ucc = web_.configuration.userContentController;
        [ucc removeAllUserScripts];
        [ucc addUserScript:[[WKUserScript alloc] initWithSource:source
                                                   injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                forMainFrameOnly:NO
                                                  inContentWorld:ThemeWorld()]];
        if (web_.URL) [web_ evaluateJavaScript:source inFrame:nil inContentWorld:ThemeWorld() completionHandler:nil];
    }

    // --- From the delegate --------------------------------------------------

    // The page has moved on (a link, a redirect, history): the address
    // bar shows where it is now, unless the user is typing in it.
    void NavigationChanged() {
        if (!alive_) return;
        if (!Editing()) address_.stringValue = OrEmpty(web_.URL.absoluteString);
        UpdateButtons();
    }

    void NavigationFailed(NSError *error) {
        if (!alive_) return;
        UpdateButtons();
        // A link that started a download, or a page left before it loaded.
        if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled) return;
        if ([error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102) return;
        NSURL *failed_url = error.userInfo[NSURLErrorFailingURLErrorKey];
        NSString *failed = failed_url ? failed_url.absoluteString : address_.stringValue;
        NSString *html = [NSString
            stringWithFormat:@"<html><body style='font:14px -apple-system;color:#888;padding:3em'><h3>Cannot open this page</h3>"
                             @"<p>%@</p><p style='word-break:break-all'>%@</p></body></html>",
                             [error.localizedDescription stringByReplacingOccurrencesOfString:@"<" withString:@"&lt;"],
                             [failed stringByReplacingOccurrencesOfString:@"<" withString:@"&lt;"]];
        [web_ loadHTMLString:html baseURL:nil];
        address_.stringValue = failed;
    }

    void AddressEntered() {
        const std::string typed = Str(address_.stringValue);
        if (Trim(typed).empty()) return;
        Load(typed);
        [host_ makeFirstResponder:web_];
    }

    // Escape in the address bar: back to what the page is, and into it.
    void AddressCancelled() {
        address_.stringValue = OrEmpty(web_.URL.absoluteString);
        [host_ makeFirstResponder:web_];
    }

    void UpdateButtons() {
        if (!alive_) return;
        back_.enabled = web_.canGoBack;
        forward_.enabled = web_.canGoForward;
        NSString *symbol = web_.loading ? @"xmark" : @"arrow.clockwise";
        reload_.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:web_.loading ? @"Stop" : @"Reload"];
        reload_.toolTip = web_.loading ? @"Stop" : @"Reload (Cmd-R)";
    }

    WKWebView *web() const { return web_; }

private:
    enum class Pending { None, Page, Address };

    // True for a responder inside this page (the web view, the address
    // field or the field editor typing into it).
    bool Inside(NSResponder *r) const {
        if (!alive_ || ![r isKindOfClass:[NSView class]]) return false;
        return [static_cast<NSView *>(r) isDescendantOf:container_];
    }

    // The theme script's own: the page cannot reach into it.
    static WKContentWorld *ThemeWorld() { return [WKContentWorld worldWithName:@"mep-theme"]; }

    bool Editing() const {
        NSResponder *r = host_.firstResponder;
        return [r isKindOfClass:[NSText class]] && static_cast<NSText *>(r).delegate == static_cast<id>(address_);
    }

    void Layout() {
        const CGFloat w = full_.w, h = full_.h;
        const CGFloat bar_h = std::min<CGFloat>(kBarHeight, h);
        bar_.frame = NSMakeRect(0, 0, w, bar_h);
        // (bar_ is not flipped: y counts from its bottom.)
        const CGFloat b = 24.0, by = (bar_h - b) / 2.0;
        back_.frame = NSMakeRect(6, by, b, b);
        forward_.frame = NSMakeRect(6 + b, by, b, b);
        reload_.frame = NSMakeRect(6 + 2 * b + 2, by, b, b);
        const CGFloat ax = 6 + 3 * b + 10;
        const CGFloat fh = 22.0;
        address_.frame = NSMakeRect(ax, (bar_h - fh) / 2.0, std::max<CGFloat>(40, w - ax - 8), fh);
        web_.frame = NSMakeRect(0, bar_h, w, std::max<CGFloat>(0, h - bar_h));
    }

    NSEvent *OnKey(NSEvent *e) {
        if (!alive_ || e.window != host_ || !Inside(host_.firstResponder)) return e;
        const NSEventModifierFlags f = e.modifierFlags;
        const unsigned mods = ((f & NSEventModifierFlagShift) ? kModShift : 0u) | ((f & NSEventModifierFlagControl) ? kModCtrl : 0u) |
                              ((f & NSEventModifierFlagOption) ? kModAlt : 0u) | ((f & NSEventModifierFlagCommand) ? kModSuper : 0u);
        // Ctrl-\ (kVK_ANSI_Backslash, or "\" wherever the layout has it):
        // the keyboard back to mep.
        if ((mods & kModCtrl) && (e.keyCode == 42 || [e.charactersIgnoringModifiers isEqualToString:@"\\"])) {
            release_request_ = true;
            return nil;
        }
        const std::string key = KeyName(e.keyCode);
        if (!key.empty() && reserved_ && reserved_(key, mods)) {
            chord_key_ = key;
            chord_mods_ = mods;
            release_request_ = true;
            return nil;
        }
        // The browser's Command keys.
        if ((mods & kModSuper) && !(mods & (kModCtrl | kModAlt))) {
            NSString *c = e.charactersIgnoringModifiers.lowercaseString;
            const bool shift = mods & kModShift;
            SEL action = nullptr;
            if ([c isEqualToString:@"l"]) {
                EditAddress();
                return nil;
            }
            if ([c isEqualToString:@"["]) {
                Back();
                return nil;
            }
            if ([c isEqualToString:@"]"]) {
                Forward();
                return nil;
            }
            if ([c isEqualToString:@"r"]) {
                if (web_.URL) [web_ reload];
                return nil;
            }
            if ([c isEqualToString:@"="] || [c isEqualToString:@"+"]) {
                web_.pageZoom = std::min<CGFloat>(web_.pageZoom * 1.1, 5.0);
                return nil;
            }
            if ([c isEqualToString:@"-"]) {
                web_.pageZoom = std::max<CGFloat>(web_.pageZoom / 1.1, 0.25);
                return nil;
            }
            if ([c isEqualToString:@"0"]) {
                web_.pageZoom = 1.0;
                return nil;
            }
            if ([c isEqualToString:@"c"]) action = @selector(copy:);
            else if ([c isEqualToString:@"v"]) action = @selector(paste:);
            else if ([c isEqualToString:@"x"]) action = @selector(cut:);
            else if ([c isEqualToString:@"a"]) action = @selector(selectAll:);
            else if ([c isEqualToString:@"z"]) action = shift ? @selector(redo:) : @selector(undo:);
            if (action) {
                [NSApp sendAction:action to:nil from:nil];
                return nil;
            }
        }
        return e;
    }

    void TearDown() {
        if (!alive_) return;
        alive_ = false;
        if (monitor_) [NSEvent removeMonitor:monitor_];
        monitor_ = nil;
        if (container_ && [host_.firstResponder isKindOfClass:[NSView class]] &&
            [static_cast<NSView *>(host_.firstResponder) isDescendantOf:container_])
            [host_ makeFirstResponder:host_.contentView];
        [web_ stopLoading];
        web_.navigationDelegate = nil;
        web_.UIDelegate = nil;
        address_.delegate = nil;
        address_.target = nil;
        back_.target = forward_.target = reload_.target = nil;
        delegate_->owner = nullptr;
        [container_ removeFromSuperview];
    }

    NSWindow *host_;
    std::function<bool(const std::string &, unsigned)> reserved_;
    MepWebDelegate *delegate_ = nil;
    MepWebContainer *container_ = nil;
    MepWebContainer *page_ = nil;
    NSView *bar_ = nil;
    NSButton *back_ = nil, *forward_ = nil, *reload_ = nil;
    NSTextField *address_ = nil;
    WKWebView *web_ = nil;
    id monitor_ = nil;
    bool alive_ = true;
    bool release_request_ = false;
    std::string chord_key_;
    unsigned chord_mods_ = 0;
    Pending pending_ = Pending::None;
    Rect full_, box_;
    // Capture's pictures: shared with WebKit's completion handler, which
    // may run after this view is gone.
    struct SnapState {
        Snapshot latest;
        bool pending = false;
        double asked_at = 0;  // systemUptime of the outstanding request
    };
    std::shared_ptr<SnapState> snap_ = std::make_shared<SnapState>();
};

std::unique_ptr<WebView> CreateWebView(Backend &, void *native_window_handle, std::function<bool(const std::string &, unsigned)> reserved,
                                       std::string *why) {
    const auto *handle = static_cast<const gfx::NativeWindowHandle *>(native_window_handle);
    NSWindow *host = handle ? (__bridge NSWindow *)handle->display : nil;
    if (!host || !host.contentView) {
        if (why) *why = "mep has no Cocoa window to show a web page in";
        return nullptr;
    }
    return std::make_unique<MacWebView>(host, std::move(reserved));
}

}  // namespace mep::gui_embed

@implementation MepWebDelegate

- (void)goBack:(id)sender {
    if (owner) owner->Back();
}
- (void)goForward:(id)sender {
    if (owner) owner->Forward();
}
- (void)reloadOrStop:(id)sender {
    if (owner) owner->Reload();
}
- (void)addressEntered:(id)sender {
    if (owner) owner->AddressEntered();
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)command {
    if (command == @selector(cancelOperation:) && owner) {
        owner->AddressCancelled();
        return YES;
    }
    return NO;
}

// --- WKNavigationDelegate ---------------------------------------------------

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    if (owner) owner->UpdateButtons();
}
- (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation {
    if (owner) owner->NavigationChanged();
}
- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    if (owner) owner->NavigationChanged();
}
- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    if (owner) owner->NavigationFailed(error);
}
- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    if (owner) owner->NavigationFailed(error);
}
- (void)webView:(WKWebView *)webView didReceiveServerRedirectForProvisionalNavigation:(WKNavigation *)navigation {
    if (owner) owner->NavigationChanged();
}
- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationAction:(WKNavigationAction *)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    // A link to something that is not the web's (mailto:, an app's own
    // scheme) goes to whatever handles it on this Mac.
    NSString *scheme = action.request.URL.scheme.lowercaseString;
    if (action.targetFrame.isMainFrame && scheme && ![@[ @"http", @"https", @"file", @"about", @"data", @"blob" ] containsObject:scheme]) {
        [[NSWorkspace sharedWorkspace] openURL:action.request.URL];
        decisionHandler(WKNavigationActionPolicyCancel);
        return;
    }
    decisionHandler(WKNavigationActionPolicyAllow);
}

// --- WKUIDelegate -------------------------------------------------------------

// target=_blank and window.open(): one page per pane, so it opens here.
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)action
                    windowFeatures:(WKWindowFeatures *)features {
    if (action.request.URL) [webView loadRequest:action.request];
    return nil;
}

- (void)webView:(WKWebView *)webView
    runJavaScriptAlertPanelWithMessage:(NSString *)message
                      initiatedByFrame:(WKFrameInfo *)frame
                     completionHandler:(void (^)(void))completionHandler {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = frame.request.URL.host ? frame.request.URL.host : @"This page says";
    alert.informativeText = message;
    [alert runModal];
    completionHandler();
}

- (void)webView:(WKWebView *)webView
    runJavaScriptConfirmPanelWithMessage:(NSString *)message
                        initiatedByFrame:(WKFrameInfo *)frame
                       completionHandler:(void (^)(BOOL))completionHandler {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = frame.request.URL.host ? frame.request.URL.host : @"This page says";
    alert.informativeText = message;
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    completionHandler([alert runModal] == NSAlertFirstButtonReturn);
}

- (void)webView:(WKWebView *)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters
              initiatedByFrame:(WKFrameInfo *)frame
             completionHandler:(void (^)(NSArray<NSURL *> *))completionHandler {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = parameters.allowsMultipleSelection;
    panel.canChooseDirectories = parameters.allowsDirectories;
    completionHandler([panel runModal] == NSModalResponseOK ? panel.URLs : nil);
}

@end

#endif  // MEP_GUI_EMBED_MACOS

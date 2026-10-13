// mep-webview: the web page behind a :Browser pane on Linux -- WebKitGTK
// (the system's WebKit, as GTK 4 ships it) in a window of its own, which
// mep's X11 embed backend takes into the pane (gui_embed_webview_x11.cpp).
//
// A program of its own rather than a WebKitGTK view inside mep's process,
// for the reasons gui_embed_webview_x11.cpp's header gives: GTK's own GL
// renderer and main loop would have to share a thread with mep's GLX
// drawing, and whatever WebKit needs of its environment (software GL,
// say) must never be mep's. In return the window is a top-level one, and
// mep adopts it the way it adopts any program's (gui_embed_x11.cpp):
// reparented into the pane, clipped to it, focused by a click or `i`,
// let go with Ctrl-\ -- a passive grab mep holds, so this program never
// sees that key.
//
// mep talks to it over its standard streams, a line each way:
//
//   stdin  (from mep)   load <target>      a URL, path, host or search, as the
//                                          address bar takes them (UrlFor)
//                       back | forward | reload
//                       address            the keyboard into the address bar
//                       theme <k=v ...>    mep's colours (ApplyTheme)
//                       close              quit
//   stdout (to mep)     url <url>          the page has moved on
//                       title <title>      its title (the host, if it has none)
//
// The window: a bar across the top (back, forward, reload/stop, the
// address) over the web view. The theme (web_theme_script.h, run in a
// script world of its own in every page) recolours the page the way
// mep's other rendered documents are; the bar is styled with the same
// colours through a GtkCssProvider.

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "web_theme_script.h"

namespace {

constexpr const char *kThemeWorld = "mep-theme";

std::string Trim(const std::string &s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsAllDigits(const std::string &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::string Str(const char *s) { return s ? std::string(s) : std::string(); }

// A line to mep. (Nothing mep is sent may hold a line break.)
void Emit(const char *kind, const std::string &value) {
    std::string v = value;
    v.erase(std::remove_if(v.begin(), v.end(), [](char c) { return c == '\n' || c == '\r'; }), v.end());
    std::printf("%s %s\n", kind, v.c_str());
    std::fflush(stdout);
}

std::string FileUri(const std::string &path) {
    gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string out = Str(uri);
    g_free(uri);
    return out.empty() ? "file://" + path : out;
}

// What the address bar (and `load`) makes of what was typed -- the same
// reading as gui_embed_webkit_macos.mm's UrlFor.
std::string UrlFor(const std::string &typed) {
    std::string t = Trim(typed);
    if (t.empty()) return "about:blank";
    if (t[0] == '~') {
        const char *home = std::getenv("HOME");
        if (home) t = std::string(home) + t.substr(1);
    }
    if (t[0] == '/') return FileUri(t);
    const std::string lower = Lower(t);
    // A scheme of its own: as it is.
    if (lower.find("://") != std::string::npos || lower.rfind("about:", 0) == 0 || lower.rfind("data:", 0) == 0 ||
        lower.rfind("file:", 0) == 0 || lower.rfind("javascript:", 0) == 0)
        return t;
    const bool has_space = t.find_first_of(" \t") != std::string::npos;
    if (!has_space) {
        // ":8000/app" and "localhost:8000": a server on this machine.
        if (t[0] == ':') return "http://localhost" + t;
        const std::string host = lower.substr(0, lower.find_first_of("/?#"));
        const std::string name = host.substr(0, host.find(':'));
        const std::string port = host.find(':') == std::string::npos ? "" : host.substr(host.find(':') + 1);
        if (name == "localhost" || name.rfind("127.", 0) == 0 || name == "0.0.0.0") return "http://" + t;
        // A file under the working directory ("index.html" is one before
        // it is a host name).
        std::error_code ec;
        if (std::filesystem::exists(t, ec)) return FileUri(std::filesystem::absolute(t, ec).string());
        // "example.com", "news.ycombinator.com/item?id=1": a host name.
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0 && dot + 2 <= name.size() && (port.empty() || IsAllDigits(port)))
            return "https://" + t;
    }
    // Anything else is a search.
    gchar *q = g_uri_escape_string(t.c_str(), nullptr, FALSE);
    const std::string out = "https://duckduckgo.com/?q=" + Str(q);
    g_free(q);
    return out;
}

// mep's theme, as the `theme` line carries it: "#rrggbb" strings and flags.
struct Theme {
    bool recolor = true, dark = false;
    std::string bg = "#ffffff", fg = "#000000", dim = "#888888", selection = "#b4d5fe", border = "#cccccc", bar = "#eeeeee";
    std::string red = "#cc0000", green = "#008800", yellow = "#aa8800", blue = "#0044cc", purple = "#8800aa", cyan = "#008888",
                orange = "#cc6600";
};

bool ParseTheme(const std::string &line, Theme *t) {
    std::vector<std::string> fields;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) fields.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) fields.push_back(cur);
    auto is_color = [](const std::string &v) {
        return v.size() == 7 && v[0] == '#' && std::all_of(v.begin() + 1, v.end(), [](unsigned char c) { return std::isxdigit(c); });
    };
    for (const std::string &f : fields) {
        const size_t eq = f.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = f.substr(0, eq), v = f.substr(eq + 1);
        if (k == "recolor") t->recolor = v == "1";
        else if (k == "dark") t->dark = v == "1";
        else if (!is_color(v)) continue;
        else if (k == "bg") t->bg = v;
        else if (k == "fg") t->fg = v;
        else if (k == "dim") t->dim = v;
        else if (k == "selection") t->selection = v;
        else if (k == "border") t->border = v;
        else if (k == "bar") t->bar = v;
        else if (k == "red") t->red = v;
        else if (k == "green") t->green = v;
        else if (k == "yellow") t->yellow = v;
        else if (k == "blue") t->blue = v;
        else if (k == "purple") t->purple = v;
        else if (k == "cyan") t->cyan = v;
        else if (k == "orange") t->orange = v;
    }
    return true;
}

// web_theme_script.h's MEP_THEME.
std::string ThemeJson(const Theme &t) {
    auto q = [](const std::string &c) { return "\"" + c + "\""; };
    return std::string("{\"recolor\":") + (t.recolor ? "true" : "false") + ",\"dark\":" + (t.dark ? "true" : "false") +
           ",\"bg\":" + q(t.bg) + ",\"fg\":" + q(t.fg) + ",\"dim\":" + q(t.dim) + ",\"selection\":" + q(t.selection) +
           ",\"border\":" + q(t.border) + ",\"pal\":{\"red\":" + q(t.red) + ",\"green\":" + q(t.green) +
           ",\"yellow\":" + q(t.yellow) + ",\"blue\":" + q(t.blue) + ",\"purple\":" + q(t.purple) + ",\"cyan\":" + q(t.cyan) +
           ",\"orange\":" + q(t.orange) + "}}";
}

std::string HtmlEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '<') out += "&lt;";
        else if (c == '&') out += "&amp;";
        else out += c;
    }
    return out;
}

struct App {
    GtkWindow *window = nullptr;
    GtkWidget *bar = nullptr;
    GtkWidget *back = nullptr, *forward = nullptr, *reload = nullptr;
    GtkWidget *address = nullptr;
    WebKitWebView *web = nullptr;
    GtkCssProvider *css = nullptr;
    GMainLoop *loop = nullptr;
    std::string stdin_buf;
    bool system_prefers_dark = false;  // GTK's own setting, before any theme
    bool address_pending = false;      // `address` before the window showed
    bool quitting = false;

    // True while the address bar is being typed into.
    bool Editing() const {
        GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window));
        return focus && (focus == address || gtk_widget_is_ancestor(focus, address));
    }

    std::string Uri() const { return Str(webkit_web_view_get_uri(web)); }

    void UpdateButtons() {
        gtk_widget_set_sensitive(back, webkit_web_view_can_go_back(web));
        gtk_widget_set_sensitive(forward, webkit_web_view_can_go_forward(web));
        const bool loading = webkit_web_view_is_loading(web);
        gtk_button_set_label(GTK_BUTTON(reload), loading ? "\xe2\x9c\x95" : "\xe2\x86\xbb");  // ✕ / ↻
        gtk_widget_set_tooltip_text(reload, loading ? "Stop" : "Reload (Ctrl-R)");
    }

    // The page has moved on (a link, a redirect, history, pushState): the
    // address bar shows where it is now, unless the user is typing in it.
    void NavigationChanged() {
        const std::string uri = Uri();
        if (!Editing() && Str(gtk_editable_get_text(GTK_EDITABLE(address))) != uri) gtk_editable_set_text(GTK_EDITABLE(address), uri.c_str());
        UpdateButtons();
        if (!uri.empty() && uri != last_url_sent) {
            last_url_sent = uri;
            Emit("url", uri);
        }
        TitleChanged();
    }

    void TitleChanged() {
        std::string t = Trim(Str(webkit_web_view_get_title(web)));
        if (t.empty()) {
            GUri *u = g_uri_parse(Uri().c_str(), G_URI_FLAGS_NONE, nullptr);
            if (u) {
                t = Str(g_uri_get_host(u));
                g_uri_unref(u);
            }
        }
        if (t.empty() && !last_title_sent.empty() && webkit_web_view_is_loading(web)) return;
        if (t != last_title_sent) {
            last_title_sent = t;
            gtk_window_set_title(window, t.empty() ? "Browser" : t.c_str());
            Emit("title", t);
        }
    }

    void Load(const std::string &target) {
        const std::string uri = UrlFor(target);
        webkit_web_view_load_uri(web, uri.c_str());
        gtk_editable_set_text(GTK_EDITABLE(address), uri.c_str());
        UpdateButtons();
    }

    void Back() {
        if (webkit_web_view_can_go_back(web)) webkit_web_view_go_back(web);
    }
    void Forward() {
        if (webkit_web_view_can_go_forward(web)) webkit_web_view_go_forward(web);
    }
    // Loads the page again, or stops it while it is still loading.
    void ReloadOrStop() {
        if (webkit_web_view_is_loading(web)) webkit_web_view_stop_loading(web);
        else if (webkit_web_view_get_uri(web)) webkit_web_view_reload(web);
        UpdateButtons();
    }

    // The keyboard into the address bar, its text selected.
    void EditAddress() {
        if (!gtk_widget_get_mapped(address)) {
            address_pending = true;
            return;
        }
        gtk_widget_grab_focus(address);
        gtk_editable_select_region(GTK_EDITABLE(address), 0, -1);
    }

    void FocusPage() { gtk_widget_grab_focus(GTK_WIDGET(web)); }

    void AddressEntered() {
        const std::string typed = Str(gtk_editable_get_text(GTK_EDITABLE(address)));
        if (Trim(typed).empty()) return;
        Load(typed);
        FocusPage();
    }

    // Escape in the address bar: back to what the page is, and into it.
    void AddressCancelled() {
        gtk_editable_set_text(GTK_EDITABLE(address), Uri().c_str());
        FocusPage();
    }

    void Zoom(double factor) {
        const double z = factor <= 0 ? 1.0 : std::clamp(webkit_web_view_get_zoom_level(web) * factor, 0.25, 5.0);
        webkit_web_view_set_zoom_level(web, z);
    }

    void ApplyTheme(const Theme &t) {
        // The bar and the address field: always mep's colours. (GTK's
        // entry is `entry > text > placeholder|selection`.)
        const std::string css_text =
            "window, #mep-page { background-color: " + t.bar + "; }\n"
            "#mep-bar { background-color: " + t.bar + "; padding: 3px 6px; }\n"
            "#mep-bar button { color: " + t.fg + "; background: none; border: none; box-shadow: none; padding: 0; min-width: 24px; "
            "min-height: 24px; font-size: 15px; -gtk-icon-shadow: none; text-shadow: none; }\n"
            "#mep-bar button:hover { background-color: alpha(" + t.fg + ", 0.12); border-radius: 4px; }\n"
            "#mep-bar button:disabled { color: " + t.dim + "; }\n"
            "#mep-address { background-color: " + t.bg + "; color: " + t.fg + "; caret-color: " + t.fg + "; border: 1px solid " +
            t.border + "; border-radius: 5px; box-shadow: none; min-height: 22px; font-size: 12px; outline: none; }\n"
            "#mep-address:focus-within { border-color: " + t.blue + "; }\n"
            "#mep-address text { color: " + t.fg + "; caret-color: " + t.fg + "; }\n"
            "#mep-address text placeholder { color: " + t.dim + "; }\n"
            "#mep-address text selection { background-color: " + t.selection + "; color: " + t.fg + "; }\n";
        gtk_css_provider_load_from_string(css, css_text.c_str());
        // The page: prefers-color-scheme says what mep's theme is (or,
        // recolouring off, what the system's is), and the script does the
        // rest -- in pages to come, and in this one.
        g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
                     static_cast<gboolean>(t.recolor ? t.dark : system_prefers_dark), nullptr);
        GdkRGBA under;
        if (!t.recolor || !gdk_rgba_parse(&under, t.bg.c_str())) gdk_rgba_parse(&under, "#ffffff");
        webkit_web_view_set_background_color(web, &under);
        const std::string source = "{ const MEP_THEME = " + ThemeJson(t) + ";\n" + kWebThemeScript + "}\n";
        WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web);
        webkit_user_content_manager_remove_all_scripts(ucm);
        WebKitUserScript *script = webkit_user_script_new_for_world(source.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, kThemeWorld, nullptr,
                                                                    nullptr);
        webkit_user_content_manager_add_script(ucm, script);
        webkit_user_script_unref(script);
        if (webkit_web_view_get_uri(web))
            webkit_web_view_evaluate_javascript(web, source.c_str(), -1, kThemeWorld, nullptr, nullptr, nullptr, nullptr);
    }

    void Quit() {
        if (quitting) return;
        quitting = true;
        webkit_web_view_stop_loading(web);
        if (loop) g_main_loop_quit(loop);
    }

    // One line from mep.
    void Command(const std::string &line_in) {
        const std::string line = Trim(line_in);
        if (line.empty()) return;
        const size_t sp = line.find(' ');
        const std::string cmd = line.substr(0, sp);
        const std::string arg = sp == std::string::npos ? "" : Trim(line.substr(sp + 1));
        if (cmd == "load") Load(arg);
        else if (cmd == "back") Back();
        else if (cmd == "forward") Forward();
        else if (cmd == "reload") ReloadOrStop();
        else if (cmd == "address") EditAddress();
        else if (cmd == "focus") FocusPage();
        else if (cmd == "theme") {
            Theme t;
            ParseTheme(arg, &t);
            ApplyTheme(t);
        } else if (cmd == "close") Quit();
        else std::fprintf(stderr, "mep-webview: unknown command '%s'\n", cmd.c_str());
    }

private:
    std::string last_url_sent, last_title_sent;
};

// --- Signals ----------------------------------------------------------------

gboolean OnStdin(gint fd, GIOCondition cond, gpointer data) {
    App *app = static_cast<App *>(data);
    if (cond & G_IO_IN) {
        char buf[4096];
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) {
                app->stdin_buf.append(buf, static_cast<size_t>(n));
                continue;
            }
            if (n == 0) {
                // mep has gone (or closed its end): so does this window.
                app->Quit();
                return G_SOURCE_REMOVE;
            }
            if (errno == EINTR) continue;
            break;  // EAGAIN: all there is for now
        }
        size_t nl;
        while ((nl = app->stdin_buf.find('\n')) != std::string::npos) {
            const std::string line = app->stdin_buf.substr(0, nl);
            app->stdin_buf.erase(0, nl + 1);
            app->Command(line);
        }
    }
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        app->Quit();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void OnLoadChanged(WebKitWebView *, WebKitLoadEvent, gpointer data) { static_cast<App *>(data)->NavigationChanged(); }
void OnUriChanged(GObject *, GParamSpec *, gpointer data) { static_cast<App *>(data)->NavigationChanged(); }
void OnTitleChanged(GObject *, GParamSpec *, gpointer data) { static_cast<App *>(data)->TitleChanged(); }
void OnLoadingChanged(GObject *, GParamSpec *, gpointer data) { static_cast<App *>(data)->UpdateButtons(); }

gboolean OnLoadFailed(WebKitWebView *web, WebKitLoadEvent, gchar *failing_uri, GError *error, gpointer data) {
    App *app = static_cast<App *>(data);
    app->UpdateButtons();
    // A link that started a download, or a page left before it loaded.
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) return TRUE;
    if (g_error_matches(error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE)) return TRUE;
    const std::string html = "<html><body style='font:14px system-ui,sans-serif;color:#888;padding:3em'><h3>Cannot open this page</h3><p>" +
                             HtmlEscape(Str(error ? error->message : nullptr)) + "</p><p style='word-break:break-all'>" +
                             HtmlEscape(Str(failing_uri)) + "</p></body></html>";
    webkit_web_view_load_alternate_html(web, html.c_str(), failing_uri, nullptr);
    gtk_editable_set_text(GTK_EDITABLE(app->address), failing_uri ? failing_uri : "");
    return TRUE;
}

gboolean OnDecidePolicy(WebKitWebView *, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer data) {
    App *app = static_cast<App *>(data);
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION || type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
        gchar *scheme_c = uri ? g_uri_parse_scheme(uri) : nullptr;
        const std::string scheme = Lower(Str(scheme_c));
        g_free(scheme_c);
        // A link to something that is not the web's (mailto:, an app's own
        // scheme) goes to whatever handles it on this desktop.
        static const char *const kWeb[] = {"http", "https", "file", "about", "data", "blob", "javascript"};
        if (!scheme.empty() && std::find(std::begin(kWeb), std::end(kWeb), scheme) == std::end(kWeb)) {
            GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
            gtk_uri_launcher_launch(launcher, app->window, nullptr, nullptr, nullptr);
            g_object_unref(launcher);
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
        return FALSE;
    }
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        // Something the view cannot show (a PDF without the plugin, an
        // archive): saved instead (OnDownloadStarted).
        if (!webkit_response_policy_decision_is_mime_type_supported(WEBKIT_RESPONSE_POLICY_DECISION(decision))) {
            webkit_policy_decision_download(decision);
            return TRUE;
        }
    }
    return FALSE;
}

// target=_blank and window.open(): one page per pane, so it opens here.
GtkWidget *OnCreate(WebKitWebView *web, WebKitNavigationAction *action, gpointer) {
    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    if (req && webkit_uri_request_get_uri(req)) webkit_web_view_load_request(web, req);
    return nullptr;
}

gboolean OnDecideDestination(WebKitDownload *download, gchar *suggested, gpointer) {
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (!dir) dir = g_get_home_dir();
    std::string name = Str(suggested);
    if (name.empty()) name = "download";
    std::filesystem::path dest = std::filesystem::path(dir) / name;
    std::error_code ec;
    for (int k = 1; std::filesystem::exists(dest, ec) && k < 1000; ++k) {
        const std::filesystem::path p(name);
        dest = std::filesystem::path(dir) / (p.stem().string() + " (" + std::to_string(k) + ")" + p.extension().string());
    }
    webkit_download_set_destination(download, dest.string().c_str());
    return TRUE;
}

void OnDownloadStarted(WebKitNetworkSession *, WebKitDownload *download, gpointer data) {
    g_signal_connect(download, "decide-destination", G_CALLBACK(OnDecideDestination), data);
}

void OnBack(GtkButton *, gpointer data) { static_cast<App *>(data)->Back(); }
void OnForward(GtkButton *, gpointer data) { static_cast<App *>(data)->Forward(); }
void OnReload(GtkButton *, gpointer data) { static_cast<App *>(data)->ReloadOrStop(); }
void OnAddressActivate(GtkEntry *, gpointer data) { static_cast<App *>(data)->AddressEntered(); }

// The browser's usual keys, wherever in the window the keyboard is. (Ctrl-\
// never arrives: mep holds a grab on it.)
gboolean OnKey(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer data) {
    App *app = static_cast<App *>(data);
    const bool ctrl = state & GDK_CONTROL_MASK, alt = state & GDK_ALT_MASK, super = state & GDK_SUPER_MASK;
    if (keyval == GDK_KEY_Escape && app->Editing()) {
        app->AddressCancelled();
        return TRUE;
    }
    if (alt && !ctrl && !super) {
        if (keyval == GDK_KEY_Left) {
            app->Back();
            return TRUE;
        }
        if (keyval == GDK_KEY_Right) {
            app->Forward();
            return TRUE;
        }
        return FALSE;
    }
    if (keyval == GDK_KEY_F5) {
        app->ReloadOrStop();
        return TRUE;
    }
    if (!ctrl || alt || super) return FALSE;
    switch (gdk_keyval_to_lower(keyval)) {
        case GDK_KEY_l: app->EditAddress(); return TRUE;
        case GDK_KEY_bracketleft: app->Back(); return TRUE;
        case GDK_KEY_bracketright: app->Forward(); return TRUE;
        case GDK_KEY_r:
            if (webkit_web_view_get_uri(app->web)) webkit_web_view_reload(app->web);
            return TRUE;
        case GDK_KEY_equal:
        case GDK_KEY_plus:
        case GDK_KEY_KP_Add: app->Zoom(1.1); return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract: app->Zoom(1.0 / 1.1); return TRUE;
        case GDK_KEY_0:
        case GDK_KEY_KP_0: app->Zoom(0); return TRUE;
        default: return FALSE;
    }
}

gboolean OnCloseRequest(GtkWindow *, gpointer data) {
    static_cast<App *>(data)->Quit();
    return TRUE;
}

// (The address bar can only take the keyboard once it is on screen.)
void OnMapped(GtkWidget *, gpointer data) {
    App *app = static_cast<App *>(data);
    if (app->address_pending) {
        app->address_pending = false;
        app->EditAddress();
    }
}

GtkWidget *BarButton(const char *label, const char *tip, GCallback cb, App *app) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_can_focus(b, FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_signal_connect(b, "clicked", cb, app);
    return b;
}

}  // namespace

int main(int argc, char **argv) {
    // The window has to be an X window for mep to take it in; and GTK's
    // client-side frame would otherwise be drawn inside the pane.
    g_setenv("GTK_CSD", "0", FALSE);
    // GIO's TLS module, where GIO would not find it on its own (Nix:
    // CMakeLists.txt's MEP_GIO_MODULES_DIR). An environment variable
    // rather than a scan here, since WebKit's network process -- the one
    // that speaks TLS -- is a process of its own that inherits this one's.
#ifdef MEP_GIO_MODULES_DIR
    if (!g_getenv("GIO_EXTRA_MODULES") && g_file_test(MEP_GIO_MODULES_DIR, G_FILE_TEST_IS_DIR))
        g_setenv("GIO_EXTRA_MODULES", MEP_GIO_MODULES_DIR, FALSE);
#endif
    // (No accessibility bus to speak to from inside a pane: GTK would
    // otherwise warn about it on every start.)
    g_setenv("GTK_A11Y", "none", FALSE);
    gdk_set_allowed_backends("x11");
    g_set_prgname("mep-webview");
    gtk_init();

    App app;
    app.loop = g_main_loop_new(nullptr, FALSE);

    gboolean prefer_dark = FALSE;
    g_object_get(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", &prefer_dark, nullptr);
    app.system_prefers_dark = prefer_dark;

    app.css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(app.css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    app.window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(app.window, "Browser");
    gtk_window_set_decorated(app.window, FALSE);
    gtk_window_set_default_size(app.window, 1024, 720);
    gtk_widget_set_name(GTK_WIDGET(app.window), "mep-page");

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(app.window, page);

    app.bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_name(app.bar, "mep-bar");
    app.back = BarButton("\xe2\x80\xb9", "Back (Ctrl-[, Alt-Left)", G_CALLBACK(OnBack), &app);          // ‹
    app.forward = BarButton("\xe2\x80\xba", "Forward (Ctrl-], Alt-Right)", G_CALLBACK(OnForward), &app);  // ›
    app.reload = BarButton("\xe2\x86\xbb", "Reload (Ctrl-R)", G_CALLBACK(OnReload), &app);              // ↻
    app.address = gtk_entry_new();
    gtk_widget_set_name(app.address, "mep-address");
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.address), "Search or enter address");
    gtk_widget_set_hexpand(app.address, TRUE);
    gtk_widget_set_valign(app.address, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(app.address, 6);
    g_signal_connect(app.address, "activate", G_CALLBACK(OnAddressActivate), &app);
    for (GtkWidget *w : {app.back, app.forward, app.reload, app.address}) gtk_box_append(GTK_BOX(app.bar), w);
    gtk_box_append(GTK_BOX(page), app.bar);

    WebKitSettings *settings = webkit_settings_new_with_settings(
        // "Inspect Element" in the page's context menu.
        "enable-developer-extras", TRUE, "javascript-can-open-windows-automatically", TRUE,
        "enable-back-forward-navigation-gestures", TRUE, "enable-smooth-scrolling", TRUE, nullptr);
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    app.web = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "settings", settings, "user-content-manager", ucm, nullptr));
    g_object_unref(settings);
    g_object_unref(ucm);
    gtk_widget_set_vexpand(GTK_WIDGET(app.web), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(app.web), TRUE);
    gtk_box_append(GTK_BOX(page), GTK_WIDGET(app.web));

    g_signal_connect(app.web, "load-changed", G_CALLBACK(OnLoadChanged), &app);
    g_signal_connect(app.web, "load-failed", G_CALLBACK(OnLoadFailed), &app);
    g_signal_connect(app.web, "decide-policy", G_CALLBACK(OnDecidePolicy), &app);
    g_signal_connect(app.web, "create", G_CALLBACK(OnCreate), &app);
    g_signal_connect(app.web, "notify::uri", G_CALLBACK(OnUriChanged), &app);
    g_signal_connect(app.web, "notify::title", G_CALLBACK(OnTitleChanged), &app);
    g_signal_connect(app.web, "notify::is-loading", G_CALLBACK(OnLoadingChanged), &app);
    g_signal_connect(webkit_web_view_get_network_session(app.web), "download-started", G_CALLBACK(OnDownloadStarted), &app);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(OnKey), &app);
    gtk_widget_add_controller(GTK_WIDGET(app.window), keys);
    g_signal_connect(app.window, "close-request", G_CALLBACK(OnCloseRequest), &app);
    g_signal_connect(app.window, "map", G_CALLBACK(OnMapped), &app);

    app.ApplyTheme(Theme{});
    app.UpdateButtons();

    // mep's commands, as they come.
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    g_unix_fd_add(STDIN_FILENO, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), OnStdin, &app);

    // Run by hand: `mep-webview <target>` shows it.
    if (argc > 1) app.Load(argv[1]);
    else gtk_widget_grab_focus(GTK_WIDGET(app.web));

    gtk_window_present(app.window);
    g_main_loop_run(app.loop);
    gtk_window_destroy(app.window);
    g_main_loop_unref(app.loop);
    return 0;
}

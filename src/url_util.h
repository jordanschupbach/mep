#ifndef MEP_URL_UTIL_H
#define MEP_URL_UTIL_H

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

// A small, dependency-free URL model shared by the browser pane's
// networking pieces: the HTTP client (what host/port/path to request),
// the resource loader and page-side fetch() (resolving a relative href
// against the document URL, same-origin checks), window.location (its
// protocol/host/pathname/... parts) and the omnibar (turning what was
// typed into something loadable).
//
// Deliberately not a WHATWG URL parser: it handles the hierarchical
// `scheme://host[:port]/path?query#fragment` shape the browser pane
// actually navigates (http, https, file), dot-segment removal and the
// reference-resolution cases a page really uses (absolute, scheme-
// relative, root-relative, path-relative, query-only, fragment-only). No
// userinfo, no IDNA/punycode, no percent-encoding normalisation -- a URL
// is carried through byte for byte apart from the path's dot segments.
namespace urlutil {

struct ParsedUrl {
    std::string scheme;    // lowercase, without "://" ("http")
    std::string host;      // lowercase ("localhost"); empty for file:
    int port = 0;          // 0 = not given (use DefaultPort)
    std::string path;      // always begins with "/" for a hierarchical URL
    std::string query;     // includes the leading "?", or empty
    std::string fragment;  // includes the leading "#", or empty
    bool valid = false;
};

/** @brief The scheme's well-known port (80 http, 443 https), or 0. */
inline int DefaultPort(const std::string &scheme) {
    if (scheme == "http") return 80;
    if (scheme == "https") return 443;
    return 0;
}

/** @brief True when `s` starts with an URL scheme followed by "://". */
inline bool HasScheme(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    return i > 0 && s.compare(i, 3, "://") == 0;
}

/** @brief True for an http:// or https:// URL. */
inline bool IsHttpUrl(const std::string &s) { return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0; }

/**
 * @brief Parses an absolute hierarchical URL.
 * @param url The URL text.
 * @return The parts; `valid` is false when there is no `scheme://`.
 */
inline ParsedUrl ParseUrl(const std::string &url) {
    ParsedUrl out;
    if (!HasScheme(url)) return out;
    const size_t scheme_end = url.find("://");
    out.scheme = url.substr(0, scheme_end);
    for (char &c : out.scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t pos = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?#", pos);
    std::string authority = url.substr(pos, authority_end == std::string::npos ? std::string::npos : authority_end - pos);
    // Drop any userinfo; the pane never sends credentials.
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        const std::string port_text = authority.substr(colon + 1);
        bool digits = !port_text.empty();
        for (char c : port_text) digits = digits && std::isdigit(static_cast<unsigned char>(c));
        if (digits) {
            out.port = std::atoi(port_text.c_str());
            authority = authority.substr(0, colon);
        }
    }
    out.host = authority;
    for (char &c : out.host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    pos = authority_end == std::string::npos ? url.size() : authority_end;
    const size_t hash = url.find('#', pos);
    const size_t query = url.find('?', pos);
    const size_t path_end = std::min(query == std::string::npos ? url.size() : query, hash == std::string::npos ? url.size() : hash);
    out.path = url.substr(pos, path_end - pos);
    if (out.path.empty()) out.path = "/";
    if (query != std::string::npos && (hash == std::string::npos || query < hash)) {
        out.query = url.substr(query, (hash == std::string::npos ? url.size() : hash) - query);
    }
    if (hash != std::string::npos) out.fragment = url.substr(hash);
    out.valid = true;
    return out;
}

/** @brief "host" or "host:port" (the port only when it isn't the scheme's default). */
inline std::string HostWithPort(const ParsedUrl &u) {
    if (u.port == 0 || u.port == DefaultPort(u.scheme)) return u.host;
    return u.host + ":" + std::to_string(u.port);
}

/** @brief "scheme://host[:port]" -- what window.location.origin reports and same-origin checks compare. */
inline std::string Origin(const ParsedUrl &u) { return u.scheme + "://" + HostWithPort(u); }

/** @brief Reassembles a URL from its parts. */
inline std::string ToString(const ParsedUrl &u) { return Origin(u) + u.path + u.query + u.fragment; }

/** @brief Removes "." and ".." segments from an absolute path (RFC 3986 5.2.4). */
inline std::string RemoveDotSegments(const std::string &path) {
    std::vector<std::string> out;
    size_t i = 0;
    const bool trailing_slash = !path.empty() && path.back() == '/';
    while (i <= path.size()) {
        const size_t next = path.find('/', i);
        const std::string seg = path.substr(i, next == std::string::npos ? std::string::npos : next - i);
        if (seg == "..") {
            if (!out.empty()) out.pop_back();
        } else if (!seg.empty() && seg != ".") {
            out.push_back(seg);
        }
        if (next == std::string::npos) break;
        i = next + 1;
    }
    std::string joined = "/";
    for (size_t k = 0; k < out.size(); k++) {
        joined += out[k];
        if (k + 1 < out.size()) joined += "/";
    }
    const bool ends_in_dots = path.size() >= 2 && (path.compare(path.size() - 2, 2, "/.") == 0 ||
                                                   (path.size() >= 3 && path.compare(path.size() - 3, 3, "/..") == 0));
    if ((trailing_slash || ends_in_dots) && joined.back() != '/') joined += "/";
    return joined;
}

/**
 * @brief Resolves a reference against an absolute base URL (RFC 3986 5.2, the cases pages use).
 * @param base The document (or other absolute) URL.
 * @param ref An absolute URL, `//host/path`, `/path`, `path`, `?query` or `#fragment`.
 * @return The absolute URL, or `ref` unchanged when `base` isn't a parseable URL.
 */
inline std::string ResolveUrl(const std::string &base, const std::string &ref) {
    if (HasScheme(ref)) {
        ParsedUrl r = ParseUrl(ref);
        if (!r.valid) return ref;
        r.path = RemoveDotSegments(r.path);
        return ToString(r);
    }
    ParsedUrl b = ParseUrl(base);
    if (!b.valid) return ref;
    if (ref.empty()) {
        b.fragment.clear();
        return ToString(b);
    }
    if (ref.rfind("//", 0) == 0) return ResolveUrl(base, b.scheme + ":" + ref);
    if (ref[0] == '#') {
        b.fragment = ref;
        return ToString(b);
    }
    if (ref[0] == '?') {
        const size_t hash = ref.find('#');
        b.query = ref.substr(0, hash);
        b.fragment = hash == std::string::npos ? "" : ref.substr(hash);
        return ToString(b);
    }
    const size_t hash = ref.find('#');
    const size_t query = ref.find('?');
    const size_t path_end = std::min(query == std::string::npos ? ref.size() : query, hash == std::string::npos ? ref.size() : hash);
    std::string path = ref.substr(0, path_end);
    if (path.empty() || path[0] != '/') {
        const size_t slash = b.path.rfind('/');
        path = b.path.substr(0, slash == std::string::npos ? 0 : slash + 1) + path;
    }
    b.path = RemoveDotSegments(path);
    b.query = (query != std::string::npos && (hash == std::string::npos || query < hash))
                  ? ref.substr(query, (hash == std::string::npos ? ref.size() : hash) - query)
                  : "";
    b.fragment = hash == std::string::npos ? "" : ref.substr(hash);
    return ToString(b);
}

/** @brief True when two absolute URLs share scheme, host and effective port. */
inline bool SameOrigin(const std::string &a, const std::string &b) {
    const ParsedUrl pa = ParseUrl(a), pb = ParseUrl(b);
    if (!pa.valid || !pb.valid) return false;
    const int port_a = pa.port ? pa.port : DefaultPort(pa.scheme);
    const int port_b = pb.port ? pb.port : DefaultPort(pb.scheme);
    return pa.scheme == pb.scheme && pa.host == pb.host && port_a == port_b;
}

/**
 * @brief Turns omnibar input into something loadable: a URL with a scheme is kept; an existing-looking
 * local path (starts with `/`, `./`, `../` or `~`) is kept; `:8080/x` becomes `http://localhost:8080/x`;
 * `localhost...`, an IPv4 address or a `host:port` gets `http://`; any other dotted host gets `https://`.
 * @param input What the user typed.
 * @return The normalized target (unchanged when nothing applies).
 */
inline std::string NormalizeOmnibarInput(const std::string &input) {
    size_t a = 0, z = input.size();
    while (a < z && std::isspace(static_cast<unsigned char>(input[a]))) a++;
    while (z > a && std::isspace(static_cast<unsigned char>(input[z - 1]))) z--;
    const std::string s = input.substr(a, z - a);
    if (s.empty() || HasScheme(s)) return s;
    if (s[0] == '/' || s[0] == '~' || s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0) return s;
    if (s[0] == ':' && s.size() > 1 && std::isdigit(static_cast<unsigned char>(s[1]))) return "http://localhost" + s;
    const size_t host_end = s.find_first_of("/?#");
    const std::string host = s.substr(0, host_end);
    const size_t colon = host.find(':');
    const std::string name = host.substr(0, colon);
    bool ipv4 = !name.empty();
    for (char c : name) ipv4 = ipv4 && (std::isdigit(static_cast<unsigned char>(c)) || c == '.');
    const bool has_port = colon != std::string::npos && colon + 1 < host.size() &&
                          std::isdigit(static_cast<unsigned char>(host[colon + 1]));
    if (name == "localhost" || ipv4 || has_port) return "http://" + s;
    if (name.find('.') != std::string::npos && name.find(' ') == std::string::npos) return "https://" + s;
    return s;
}

}  // namespace urlutil

#endif  // MEP_URL_UTIL_H

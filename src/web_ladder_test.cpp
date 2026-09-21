// Headless test of the browser pane's networking, plus the browser-
// capability ladder run through mep's own engine.
//
// Part 1 (CHECK-based, must pass): url_util.h's parsing/resolution/omnibar
// normalization, and a real round trip between HttpStaticServer and
// HttpGet over 127.0.0.1 -- status codes, content types, index/redirect/
// listing behaviour, root confinement, HEAD, the unsupported-method answer.
//
// Part 2 (a report, not a gate): serves examples/web and loads every level
// the way the pane does -- HttpGet the page, ParseHtml, LoadRemoteHtml-
// Resources against its URL, RunScripts -- then prints each page's own
// verdict (its <title>, written by shared/harness.js) and its failed
// checks. A level that passes in a real browser (examples/web/tools/
// baseline.mjs) and fails here names a missing browser feature; that is
// the whole point of the ladder, so a failing level does not fail this
// binary unless --strict is given.
//
//   mep-web-ladder-test [--strict] [path/to/examples/web]
//   mep-web-ladder-test --eval script.js
//   mep-web-ladder-test --level 12        (one level, with console output and every error)
#include <chrono>
#include <thread>
#include <memory>
#include "html_doc.h"
#include "http_client.h"
#include "http_server.h"
#include "js_engine.h"
#include "url_util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
// Plays the pane's frame loop: keep pumping timers, animation frames and
// promise jobs until the page has nothing scheduled, its harness verdict
// is final (no longer "RUNNING"), or the wall-clock budget runs out.
void PumpUntilSettled(JsRuntime &runtime, HtmlDoc &doc, int budget_ms) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        PumpScripts(runtime);
        const bool running = doc.title.rfind("RUNNING", 0) == 0;
        const double wake = ScriptsNextWakeMs(runtime);
        if (wake < 0) break;                      // nothing scheduled: the page is idle
        if (!running && wake > 1500) break;       // verdict is in; only slow background timers remain
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (elapsed > budget_ms) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(std::min(wake, 16.0))));
    }
}

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

void CollectText(const DomNode *node, std::string &out) {
    if (!node) return;
    if (node->type == DomNodeType::Text) out += node->text;
    for (const auto &child : node->children) CollectText(child.get(), out);
}

// Every <li class="fail"> in the document -- the harness's own failure rows.
// Deliberately not scoped to #results: whether `el.id = 'results'` reflects
// into the attribute is itself one of the things an engine can get wrong.
void CollectFailures(const DomNode *node, bool inside_results, std::vector<std::string> &out) {
    if (!node) return;
    const bool in_results = true;
    (void)inside_results;
    if (in_results && node->type == DomNodeType::Element && node->tag == "li") {
        auto cls = node->attrs.find("class");
        if (cls != node->attrs.end() && cls->second == "fail") {
            std::string text;
            CollectText(node, text);
            out.push_back(text);
        }
    }
    for (const auto &child : node->children) CollectFailures(child.get(), in_results, out);
}

void TestUrlUtil() {
    using namespace urlutil;
    const ParsedUrl u = ParseUrl("HTTP://LocalHost:8080/a/b.html?x=1&y=2#frag");
    CHECK(u.valid && u.scheme == "http" && u.host == "localhost" && u.port == 8080);
    CHECK(u.path == "/a/b.html" && u.query == "?x=1&y=2" && u.fragment == "#frag");
    CHECK(Origin(u) == "http://localhost:8080");
    CHECK(Origin(ParseUrl("http://example.com:80/x")) == "http://example.com");
    CHECK(ParseUrl("http://host").path == "/");
    CHECK(!ParseUrl("no-scheme/path").valid);
    const std::string base = "http://localhost:8000/app/pages/index.html?old=1#top";
    CHECK(ResolveUrl(base, "style.css") == "http://localhost:8000/app/pages/style.css");
    CHECK(ResolveUrl(base, "../shared/harness.js") == "http://localhost:8000/app/shared/harness.js");
    CHECK(ResolveUrl(base, "../../../../etc/passwd") == "http://localhost:8000/etc/passwd");
    CHECK(ResolveUrl(base, "/root.js") == "http://localhost:8000/root.js");
    CHECK(ResolveUrl(base, "?view=about") == "http://localhost:8000/app/pages/index.html?view=about");
    CHECK(ResolveUrl(base, "#section") == "http://localhost:8000/app/pages/index.html?old=1#section");
    CHECK(ResolveUrl(base, "//cdn.example.com/lib.js") == "http://cdn.example.com/lib.js");
    CHECK(ResolveUrl(base, "https://other.test/x?q=1") == "https://other.test/x?q=1");
    CHECK(ResolveUrl(base, "./a/./b/../c.png") == "http://localhost:8000/app/pages/a/c.png");
    CHECK(ResolveUrl("http://localhost:8000/dir/", "x.json") == "http://localhost:8000/dir/x.json");
    CHECK(SameOrigin("http://localhost:8000/a", "http://LOCALHOST:8000/b?c"));
    CHECK(!SameOrigin("http://localhost:8000/a", "http://localhost:8001/a"));
    CHECK(!SameOrigin("http://localhost/a", "https://localhost/a"));
    CHECK(SameOrigin("http://example.com/a", "http://example.com:80/b"));
    CHECK(NormalizeOmnibarInput("localhost:8000") == "http://localhost:8000");
    CHECK(NormalizeOmnibarInput("  localhost:8000/app/  ") == "http://localhost:8000/app/");
    CHECK(NormalizeOmnibarInput(":5173/x") == "http://localhost:5173/x");
    CHECK(NormalizeOmnibarInput("127.0.0.1:3000") == "http://127.0.0.1:3000");
    CHECK(NormalizeOmnibarInput("example.com/docs") == "https://example.com/docs");
    CHECK(NormalizeOmnibarInput("https://a.test/") == "https://a.test/");
    CHECK(NormalizeOmnibarInput("/home/me/page.html") == "/home/me/page.html");
    CHECK(NormalizeOmnibarInput("./page.html") == "./page.html");
    CHECK(NormalizeOmnibarInput("notes") == "notes");
}

void TestServerAndClient() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("mep-http-test-" + std::to_string(std::rand()));
    fs::create_directories(root / "sub" / "empty");
    fs::create_directories(root / "listing");
    std::ofstream(root / "index.html") << "<!DOCTYPE html><title>root</title><p>hello</p>";
    std::ofstream(root / "sub" / "data.json") << "{\"n\": 42}";
    std::ofstream(root / "sub" / "app.js") << "window.x = 1;";
    std::ofstream(root / "sub" / "with space.txt") << "spaced";
    std::ofstream(root / "listing" / "a.txt") << "a";
    std::ofstream(root.parent_path() / (root.filename().string() + "-secret.txt")) << "outside";

    HttpStaticServer server;
    std::string error;
    CHECK(server.Start(root.string(), 0, &error));
    CHECK(server.Running() && server.Port() > 0);
    const std::string base = "http://127.0.0.1:" + std::to_string(server.Port());

    HttpResponse r = HttpGet(base + "/");
    CHECK(r.ok && r.status == 200 && r.ContentType() == "text/html");
    CHECK(r.body.find("<p>hello</p>") != std::string::npos);
    CHECK(r.headers["cache-control"] == "no-store");

    r = HttpGet(base + "/sub/data.json");
    CHECK(r.ok && r.status == 200 && r.ContentType() == "application/json" && r.body == "{\"n\": 42}");
    r = HttpGet(base + "/sub/app.js?cachebust=1");
    CHECK(r.ok && r.status == 200 && r.ContentType() == "text/javascript" && r.body == "window.x = 1;");
    r = HttpGet(base + "/sub/with%20space.txt");
    CHECK(r.ok && r.status == 200 && r.body == "spaced");

    r = HttpGet(base + "/missing.html");
    CHECK(r.ok && r.status == 404);

    // A directory without the slash redirects to the slash form (followed by the client).
    r = HttpGet(base + "/listing");
    CHECK(r.ok && r.status == 200 && r.url == base + "/listing/");
    CHECK(r.body.find("Index of /listing/") != std::string::npos && r.body.find("a.txt") != std::string::npos);

    // Confinement: neither dot segments nor an encoded traversal escape the root.
    r = HttpGet(base + "/../" + root.filename().string() + "-secret.txt");
    CHECK(r.ok && r.status == 404);  // the client normalizes the path; it's simply not under root
    r = HttpGet(base + "/%2e%2e/" + root.filename().string() + "-secret.txt");
    CHECK(r.ok && r.status == 403);
    CHECK(r.body.find("outside") == std::string::npos);

    // Nothing is listening on a closed port: a transport failure, not a status.
    const int dead_port = server.Port();
    server.Stop();
    CHECK(!server.Running());
    r = HttpGet("http://127.0.0.1:" + std::to_string(dead_port) + "/", 1500);
    CHECK(!r.ok && r.status == 0 && !r.error.empty());
    r = HttpGet("ftp://127.0.0.1/x");
    CHECK(!r.ok);

    // A fixed port that's taken fails cleanly; a second server on port 0 coexists.
    HttpStaticServer a, b;
    CHECK(a.Start(root.string(), 0, &error));
    CHECK(!b.Start(root.string(), a.Port(), &error) && !error.empty());
    CHECK(b.Start(root.string(), 0, &error) && b.Port() != a.Port());
    CHECK(!HttpStaticServer().Start((root / "index.html").string(), 0, &error));  // not a directory

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove(root.parent_path() / (root.filename().string() + "-secret.txt"), ec);
    CHECK(HttpContentTypeForPath("x/Y.PNG") == "image/png" && HttpContentTypeForPath("noext") == "application/octet-stream");
}

int RunLadder(const std::string &dir, bool strict, const std::string &only = "") {
    namespace fs = std::filesystem;
    if (!fs::is_directory(dir)) {
        std::printf("ladder: %s not found, skipping\n", dir.c_str());
        return 0;
    }
    SetHtmlUrlFetcher([](const std::string &url) {
        HtmlFetchResult out;
        HttpResponse response = HttpGet(url, 10000);
        if (!response.ok) {
            out.error = response.error;
            return out;
        }
        out.status = response.status;
        out.content_type = response.ContentType();
        out.url = response.url;
        out.body = std::move(response.body);
        return out;
    });
    HttpStaticServer server;
    std::string error;
    if (!server.Start(dir, 0, &error)) {
        std::printf("ladder: cannot serve %s: %s\n", dir.c_str(), error.c_str());
        return strict ? 1 : 0;
    }
    std::vector<std::string> levels;
    for (const auto &entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && name.size() > 3 && std::isdigit(static_cast<unsigned char>(name[0])) &&
            std::isdigit(static_cast<unsigned char>(name[1])) && name[2] == '-') {
            levels.push_back(name);
        }
    }
    std::sort(levels.begin(), levels.end());
    int passed = 0;
    std::printf("\nBrowser capability ladder through mep's engine (%s)\n", dir.c_str());
    for (const std::string &level : levels) {
        if (!only.empty() && level.find(only) == std::string::npos) continue;
        const std::string url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/" + level + "/index.html";
        HttpResponse page = HttpGet(url);
        std::string verdict, first_error;
        std::vector<std::string> failures;
        bool ok = false;
        if (!page.ok || page.status != 200) {
            verdict = "(page did not load: " + (page.ok ? std::to_string(page.status) : page.error) + ")";
        } else {
            HtmlDoc doc;
            ParseHtml(page.body, doc);
            doc.document_url = url;
            LoadRemoteHtmlResources(doc, url);
            // --level NAME: just that level, with its console output and every error.
            const bool verbose = !only.empty();
            std::shared_ptr<JsRuntime> runtime = StartScripts(doc, [verbose](const std::string &msg) { if (verbose) std::printf("       console: %s\n", msg.c_str()); }, [&](const std::string &msg) {
                if (verbose) std::printf("       error: %s\n", msg.c_str());
                if (first_error.empty()) first_error = msg;
            });
            PumpUntilSettled(*runtime, doc, 8000);
            verdict = doc.title;
            CollectFailures(doc.root.get(), false, failures);
            // 01-static has no script: it passes if it parsed and its stylesheet was folded in.
            ok = level.rfind("01-", 0) == 0 ? !doc.title.empty() : verdict.rfind("PASS", 0) == 0;
        }
        if (ok) passed++;
        std::printf("%s %-14s %s\n", ok ? "ok  " : "FAIL", level.c_str(), verdict.c_str());
        for (const std::string &f : failures) std::printf("       - %s\n", f.c_str());
        if (!first_error.empty()) std::printf("       ! %s\n", first_error.c_str());
    }
    std::printf("%d of %zu levels pass in mep\n", passed, levels.size());
    return strict && passed != static_cast<int>(levels.size()) ? 1 : 0;
}


// --eval file.js: runs one script in an empty document and prints what the
// engine said -- console output, the first error, the resulting title. The
// quickest way to find which construct of a failing page the parser or
// interpreter rejects (bisect the file, re-run).
int EvalFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("cannot read %s\n", path.c_str());
        return 2;
    }
    std::string code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    HtmlDoc doc;
    ParseHtml("<!DOCTYPE html><html><head><title>eval</title></head><body><div id=\"root\"></div></body></html>", doc);
    doc.scripts.push_back(code);
    int errors = 0;
    std::shared_ptr<JsRuntime> runtime = StartScripts(doc, [](const std::string &msg) { std::printf("console: %s\n", msg.c_str()); },
                                                       [&](const std::string &msg) {
                                                           errors++;
                                                           std::printf("error: %s\n", msg.c_str());
                                                       });
    PumpUntilSettled(*runtime, doc, 3000);
    std::printf("title: %s\n%s\n", doc.title.c_str(), errors ? "FAILED" : "ok");
    return errors ? 1 : 0;
}
}  // namespace

int main(int argc, char **argv) {
    bool strict = false;
    std::string only;
    std::string dir = std::string(MEP_SOURCE_DIR) + "/examples/web";
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--strict") == 0) strict = true;
        else if (std::strcmp(argv[i], "--eval") == 0 && i + 1 < argc) return EvalFile(argv[i + 1]);
        else if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) only = argv[++i];
        else dir = argv[i];
    }
    TestUrlUtil();
    TestServerAndClient();
    std::printf("url_util + http server/client: ok\n");
    return RunLadder(dir, strict, only);
}

// grammar.js -> grammar.json on mep's own JS engine: see generate.h.
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

#include "generate.h"
#include "html_doc.h"
#include "js_engine.h"
#include "ts_generate_embedded.h"

namespace fs = std::filesystem;

namespace tsgen {

namespace {

std::string ReadText(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) Fail("cannot read " + path.string());
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// A JS string literal holding `text`.
std::string Quote(const std::string &text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// Defines `path` and, depth first, every module its require() calls name:
// literal relative paths, as every grammar's are.
void DefineModule(const fs::path &path, std::map<fs::path, bool> &seen, std::string &script) {
    if (seen[path]) return;
    seen[path] = true;
    const std::string source = ReadText(path);
    if (path.extension() == ".json") {
        script += "__mepDefine(" + Quote(path.string()) + ", {}, " + source + ");\n";
        return;
    }
    static const std::regex kRequire(R"(require\(\s*['"]([^'"]+)['"]\s*\))");
    std::string deps;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), kRequire); it != std::sregex_iterator(); ++it) {
        const std::string name = (*it)[1];
        if (name.rfind("./", 0) != 0 && name.rfind("../", 0) != 0) Fail(path.string() + ": cannot require '" + name + "' (only relative paths)");
        fs::path dep = fs::weakly_canonical(path.parent_path() / name);
        if (!fs::exists(dep) && fs::exists(dep.string() + ".js")) dep += ".js";
        DefineModule(dep, seen, script);
        deps += (deps.empty() ? "" : ", ") + Quote(name) + ": " + Quote(dep.string());
    }
    script += "__mepDefine(" + Quote(path.string()) + ", {" + deps + "}, function (module, exports, require) {\n" + source +
              "\n});\n";
}

}  // namespace

std::string EvaluateGrammarJs(const fs::path &grammar_js) {
    const fs::path main = fs::weakly_canonical(grammar_js);
    const std::string error_prefix = "mep-ts-generate error: ";
    std::string script = kDslJs;
    script += "const kError = " + Quote(error_prefix) + ";\n";
    std::map<fs::path, bool> seen;
    DefineModule(main, seen, script);
    // An uncaught error would be reported by name only: catch it for its message.
    script += "try { console.log(__mepGrammarJson(" + Quote(main.string()) +
              R"()); } catch (e) { console.log(kError + (e && e.message !== undefined ? e.message : e)); })" "\n";

    HtmlDoc doc;
    ParseHtml("", doc, true, false);
    doc.scripts = {script};
    doc.script_info.clear();
    std::string json, error;
    RunScripts(
        doc, [&](const std::string &line) { json = line; },
        [&](const std::string &message) { error = message; });
    if (json.rfind(error_prefix, 0) == 0) error = json.substr(error_prefix.size());
    if (!error.empty()) Fail("Failed to evaluate " + grammar_js.string() + " -- " + error);
    if (json.empty()) Fail("Failed to evaluate " + grammar_js.string() + " -- no output");
    return json;
}

}  // namespace tsgen

// mep-ts-generate: tree-sitter's parser generator, for mep's own grammars
// (plans/TREESITTER_GRAMMAR_PLAN.md Phase 9). Given a grammar directory, it
// writes what `tree-sitter generate` 0.25.10 does, byte for byte:
// src/grammar.json from grammar.js (run on mep's JS engine, not Node), then
// src/parser.c, src/node-types.json and src/tree_sitter/{alloc,array,
// parser}.h from it.
//
//   mep-ts-generate [--abi N] [--out DIR] [--json FILE] GRAMMAR_DIR
//
// --json FILE generates from that grammar.json instead of grammar.js (and
// GRAMMAR_DIR without a grammar.js uses its committed src/grammar.json).
// --out DIR writes everything, grammar.json included, to DIR, not src/.
//
// The parser's ABI version (default 15) needs the grammar's version, read
// from the nearest tree-sitter.json at or above GRAMMAR_DIR, as the CLI
// does; without one it falls back to ABI 14.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "generate.h"
#include "ts_generate_embedded.h"

namespace fs = std::filesystem;
using namespace tsgen;

namespace {

std::string ReadFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) Fail("cannot read " + path.string());
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void WriteFile(const fs::path &path, const std::string &body) {
    std::ofstream out(path, std::ios::binary);
    if (!out) Fail("Failed to write " + path.string());
    out << body;
}

// read_grammar_version: the first tree-sitter.json at or above `dir`.
std::optional<SemanticVersion> ReadGrammarVersion(fs::path dir) {
    dir = fs::absolute(dir);
    while (true) {
        fs::path candidate = dir / "tree-sitter.json";
        if (fs::exists(candidate)) {
            JsonValue json;
            std::string error;
            if (!ParseJson(ReadFile(candidate), json, error)) Fail("Failed to parse `" + candidate.string() + "` -- " + error);
            const JsonValue *metadata = json.Get("metadata");
            const JsonValue *version = metadata ? metadata->Get("version") : nullptr;
            if (!version || version->type != JsonValue::Type::String)
                Fail("Failed to parse `" + candidate.string() + "` -- missing field `version`");
            unsigned major = 0, minor = 0, patch = 0;
            char rest = 0;
            int n = std::sscanf(version->string.c_str(), "%u.%u.%u%c", &major, &minor, &patch, &rest);
            if (n < 3 || major > 255 || minor > 255 || patch > 255)
                Fail("Failed to parse `" + candidate.string() + "` version as semver");
            return SemanticVersion{static_cast<uint8_t>(major), static_cast<uint8_t>(minor), static_cast<uint8_t>(patch)};
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) return std::nullopt;
        dir = dir.parent_path();
    }
}

[[noreturn]] void Usage() {
    std::fprintf(stderr, "usage: mep-ts-generate [--abi N] [--out DIR] [--json FILE] GRAMMAR_DIR\n");
    std::exit(2);
}

}  // namespace

int main(int argc, char **argv) {
    size_t abi_version = 15;
    std::optional<fs::path> out_dir, json_path;
    std::optional<fs::path> grammar_dir;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--abi" && i + 1 < argc) abi_version = std::stoul(argv[++i]);
        else if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (arg == "--json" && i + 1 < argc) json_path = argv[++i];
        else if (arg.size() > 1 && arg[0] == '-') Usage();
        else if (!grammar_dir) grammar_dir = arg;
        else Usage();
    }
    if (!grammar_dir) Usage();
    fs::path src = out_dir ? *out_dir : *grammar_dir / "src";
    std::string grammar_json;
    if (json_path) grammar_json = ReadFile(*json_path);
    else if (fs::exists(*grammar_dir / "grammar.js")) grammar_json = EvaluateGrammarJs(*grammar_dir / "grammar.js");
    else grammar_json = ReadFile(*grammar_dir / "src" / "grammar.json");

    JsonValue json;
    std::string error;
    if (!ParseJson(grammar_json, json, error)) Fail("Failed to load grammar.json -- " + error);
    InputGrammar input = ParseGrammar(json);
    std::optional<SemanticVersion> version = ReadGrammarVersion(*grammar_dir);
    if (!version && abi_version > 14) {
        std::printf("Warning: No `tree-sitter.json` file found in your grammar, this file is required to generate with ABI %zu. "
                    "Using ABI version 14 instead.\n",
                    abi_version);
        abi_version = 14;
    }

    PreparedGrammar prepared = PrepareGrammar(input);
    std::vector<VariableInfo> variable_info = GetVariableInfo(prepared.syntax, prepared.lexical, prepared.simple_aliases);
    JsonValue node_types = GenerateNodeTypesJson(prepared.syntax, prepared.lexical, prepared.simple_aliases, variable_info);
    auto supertype_symbol_map = GetSupertypeSymbolMap(prepared.syntax, prepared.simple_aliases, variable_info);
    Tables tables = BuildTables(prepared.syntax, prepared.lexical, prepared.simple_aliases, variable_info, prepared.inlines);
    std::string c_code = RenderC(input.name, std::move(tables), std::move(prepared.syntax), std::move(prepared.lexical),
                                 std::move(prepared.simple_aliases), abi_version, version, std::move(supertype_symbol_map));

    fs::create_directories(src / "tree_sitter");
    if (!json_path) WriteFile(src / "grammar.json", grammar_json);
    WriteFile(src / "parser.c", c_code);
    WriteFile(src / "node-types.json", PrettyJson(node_types));
    WriteFile(src / "tree_sitter" / "alloc.h", kTemplateAllocH);
    WriteFile(src / "tree_sitter" / "array.h", kTemplateArrayH);
    WriteFile(src / "tree_sitter" / "parser.h", kTemplateParserH);
    return 0;
}

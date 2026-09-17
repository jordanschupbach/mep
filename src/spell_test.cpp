// Coverage for spell.h/.cpp: the dependency-free spell checker's edit-distance
// and Soundex helpers, case-insensitive IsMisspelled with its skip rules
// (digits, punctuation, acronyms) and personal good/wrong overrides, Suggest
// ranking + capitalization matching, and that AddGood/AddWrong both take effect
// immediately and persist to the personal spellfiles. Builds a tiny in-memory
// wordlist written to a temp file rather than depending on the bundled asset.

#include "spell.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

std::filesystem::path WriteTemp(const std::string &name, const std::string &contents) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::ofstream out(p);
    out << contents;
    out.close();
    return p;
}

bool Has(const std::vector<std::string> &v, const std::string &s) {
    for (const std::string &e : v)
        if (e == s) return true;
    return false;
}

void TestHelpers() {
    using namespace spell;
    CHECK(ToLowerAscii("TeH") == "teh");
    CHECK(EditDistance("teh", "the", 2) == 1);      // adjacent transposition = 1 (Damerau/OSA)
    CHECK(EditDistance("recieve", "receive", 2) == 1);
    CHECK(EditDistance("kitten", "sitting", 3) == 3);  // 2 subs + 1 insert
    CHECK(EditDistance("the", "the", 2) == 0);
    CHECK(EditDistance("cat", "elephant", 2) == 3);  // pruned to max_dist+1
    // Soundex: classic reference values.
    CHECK(Soundex("Robert") == "R163");
    CHECK(Soundex("Rupert") == "R163");
    CHECK(Soundex("Tymczak") == "T522");
    CHECK(Soundex("") == "");
    // Case matching.
    CHECK(MatchCase("the", "Teh") == "The");
    CHECK(MatchCase("the", "TEH") == "THE");
    CHECK(MatchCase("the", "teh") == "the");
}

void TestChecking() {
    using namespace spell;
    SpellChecker sp;
    CHECK(!sp.ready());
    CHECK(!sp.IsMisspelled("anything"));  // not ready -> nothing flagged

    auto dict = WriteTemp("mep_spell_dict.txt",
                          "the\nquick\nbrown\nfox\nreceive\nunknown\ncolour\nParis\n");
    CHECK(sp.LoadDictionary(dict.string()));
    CHECK(sp.ready());

    CHECK(!sp.IsMisspelled("the"));
    CHECK(!sp.IsMisspelled("The"));     // case-insensitive
    CHECK(!sp.IsMisspelled("THE"));
    CHECK(sp.IsMisspelled("teh"));
    CHECK(sp.IsMisspelled("recieve"));
    CHECK(!sp.IsMisspelled("receive"));
    CHECK(!sp.IsMisspelled("paris"));   // dict entry "Paris" matches lowercased
    // Skip rules: digits, punctuation-only, acronyms.
    CHECK(!sp.IsMisspelled("h4x"));      // contains a digit
    CHECK(!sp.IsMisspelled("!!!"));      // no letters
    CHECK(!sp.IsMisspelled("NASA"));     // all-caps acronym ignored by default
    sp.set_ignore_uppercase(false);
    CHECK(sp.IsMisspelled("NASA"));      // ...unless disabled
    sp.set_ignore_uppercase(true);

    std::filesystem::remove(dict);
}

void TestSuggest() {
    using namespace spell;
    SpellChecker sp;
    // Includes decoys that would win on a naive alphabetical/length tie-break:
    // "Te"/"tea"/"tee" for "teh", "Born"/"born" for "borwn". The common-word
    // frequency tier + same-length + case-compat ranking must still pick the
    // genuinely-likeliest word.
    auto dict = WriteTemp("mep_spell_dict2.txt",
                          "Te\ntea\ntee\nthe\nthen\nthee\nquick\nreceive\nreceiver\n"
                          "Born\nborn\nbrown\nphone\n");
    CHECK(sp.LoadDictionary(dict.string()));

    auto s1 = sp.Suggest("teh");
    CHECK(!s1.empty());
    CHECK(s1[0] == "the");  // common-word tier beats "Te"/"tea"/"tee"

    auto s2 = sp.Suggest("recieve");
    CHECK(!s2.empty());
    CHECK(s2[0] == "receive");

    auto sb = sp.Suggest("borwn");
    CHECK(!sb.empty());
    CHECK(sb[0] == "brown");  // same-length + lowercase beats "Born"

    // Capitalization of the input carries to the suggestion.
    auto s3 = sp.Suggest("Teh");
    CHECK(!s3.empty());
    CHECK(s3[0] == "The");

    // A correct word yields itself only via others, never itself.
    auto s4 = sp.Suggest("the");
    CHECK(!Has(s4, "the"));

    std::filesystem::remove(dict);
}

void TestPersonalDictionary() {
    using namespace spell;
    auto dir = std::filesystem::temp_directory_path() / "mep_spell_personal";
    std::filesystem::remove_all(dir);
    auto good = dir / "en.add";
    auto wrong = dir / "en.wrong";

    auto dict = WriteTemp("mep_spell_dict3.txt", "the\ncolour\nfoobar\n");

    {
        SpellChecker sp;
        CHECK(sp.LoadDictionary(dict.string()));
        sp.SetPersonalFiles(good.string(), wrong.string());

        CHECK(sp.IsMisspelled("mepml"));   // unknown word
        sp.AddGood("mepml");
        CHECK(!sp.IsMisspelled("mepml"));  // now accepted immediately
        CHECK(std::filesystem::exists(good));  // ...and persisted

        CHECK(!sp.IsMisspelled("foobar"));  // in main dict
        sp.AddWrong("foobar");
        CHECK(sp.IsMisspelled("foobar"));   // user override wins
        CHECK(std::filesystem::exists(wrong));
    }
    {
        // A fresh checker loading the same personal files sees the changes.
        SpellChecker sp;
        CHECK(sp.LoadDictionary(dict.string()));
        sp.SetPersonalFiles(good.string(), wrong.string());
        CHECK(!sp.IsMisspelled("mepml"));
        CHECK(sp.IsMisspelled("foobar"));
    }

    std::filesystem::remove(dict);
    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    TestHelpers();
    TestChecking();
    TestSuggest();
    TestPersonalDictionary();
    std::printf("spell_test: all checks passed\n");
    return 0;
}

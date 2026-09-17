#include "spell.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace spell {

namespace {

bool HasAsciiLetter(std::string_view s) {
    for (char c : s)
        if (std::isalpha(static_cast<unsigned char>(c))) return true;
    return false;
}

bool HasDigit(std::string_view s) {
    for (char c : s)
        if (std::isdigit(static_cast<unsigned char>(c))) return true;
    return false;
}

bool IsAllUpper(std::string_view s) {
    bool any = false;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::islower(u)) return false;
        if (std::isupper(u)) any = true;
    }
    return any;
}

}  // namespace

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char &c : out) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 128) c = static_cast<char>(std::tolower(u));
    }
    return out;
}

int EditDistance(std::string_view a, std::string_view b, int max_dist) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (std::abs(static_cast<int>(n) - static_cast<int>(m)) > max_dist) return max_dist + 1;
    // Optimal String Alignment (restricted Damerau-Levenshtein): like plain
    // Levenshtein but an adjacent transposition (the single most common typo,
    // e.g. "teh"->"the", "recieve"->"receive") costs 1 rather than 2. Needs
    // three rolling rows so cur[j] can reach two back via prevprev[j-2].
    std::vector<int> prevprev(m + 1, 0), prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        int row_min = cur[0];
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int v = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i >= 2 && j >= 2 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                v = std::min(v, prevprev[j - 2] + 1);
            cur[j] = v;
            row_min = std::min(row_min, v);
        }
        if (row_min > max_dist) return max_dist + 1;  // whole row too far -> prune
        prevprev.swap(prev);
        prev.swap(cur);
    }
    return prev[m];
}

std::string Soundex(std::string_view word) {
    // Map a letter to its Soundex digit ('0' = vowel/ignored group).
    auto code = [](char c) -> char {
        switch (std::toupper(static_cast<unsigned char>(c))) {
            case 'B': case 'F': case 'P': case 'V': return '1';
            case 'C': case 'G': case 'J': case 'K':
            case 'Q': case 'S': case 'X': case 'Z': return '2';
            case 'D': case 'T': return '3';
            case 'L': return '4';
            case 'M': case 'N': return '5';
            case 'R': return '6';
            default: return '0';  // A E I O U H W Y and non-letters
        }
    };
    // Find first ASCII letter.
    size_t i = 0;
    while (i < word.size() && !std::isalpha(static_cast<unsigned char>(word[i]))) ++i;
    if (i >= word.size()) return "";
    std::string out;
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(word[i])));
    char last = code(word[i]);
    for (size_t k = i + 1; k < word.size() && out.size() < 4; ++k) {
        if (!std::isalpha(static_cast<unsigned char>(word[k]))) continue;
        char d = code(word[k]);
        // Same code as previous consonant collapses; a vowel resets so a later
        // repeat is kept (standard Soundex behaviour). H and W do not reset.
        char skipped = static_cast<char>(std::toupper(static_cast<unsigned char>(word[k - 1])));
        if (d != '0' && d != last) out += d;
        if (skipped != 'H' && skipped != 'W') last = d;
    }
    while (out.size() < 4) out += '0';
    return out;
}

std::string MatchCase(std::string_view suggestion, std::string_view pattern) {
    std::string out(suggestion);
    if (IsAllUpper(pattern)) {
        for (char &c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }
    if (!pattern.empty() && std::isupper(static_cast<unsigned char>(pattern.front())) && !out.empty()) {
        out.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(out.front())));
    }
    return out;
}

void DefaultPersonalPaths(std::string *good_out, std::string *wrong_out) {
    std::string base;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = std::string(xdg) + "/mep/spell";
    } else if (const char *home = std::getenv("HOME"); home && *home) {
        base = std::string(home) + "/.config/mep/spell";
    }
    if (base.empty()) {
        if (good_out) good_out->clear();
        if (wrong_out) wrong_out->clear();
        return;
    }
    if (good_out) *good_out = base + "/en.add";
    if (wrong_out) *wrong_out = base + "/en.wrong";
}

namespace {
// The ~200 most frequent English words (roughly descending frequency). Used
// only as the primary suggestion tie-break after edit distance, so that a
// typo of a common function word ("teh"->"the", "adn"->"and", "fro"->"for")
// resolves to the likeliest word rather than an alphabetically-earlier rare
// one. Not a dictionary (the bundled wordlist is), just a frequency hint.
const char *const kCommonWords[] = {
    "the", "be", "to", "of", "and", "a", "in", "that", "have", "I", "it", "for", "not", "on", "with",
    "he", "as", "you", "do", "at", "this", "but", "his", "by", "from", "they", "we", "say", "her", "she",
    "or", "an", "will", "my", "one", "all", "would", "there", "their", "what", "so", "up", "out", "if",
    "about", "who", "get", "which", "go", "me", "when", "make", "can", "like", "time", "no", "just", "him",
    "know", "take", "people", "into", "year", "your", "good", "some", "could", "them", "see", "other",
    "than", "then", "now", "look", "only", "come", "its", "over", "think", "also", "back", "after", "use",
    "two", "how", "our", "work", "first", "well", "way", "even", "new", "want", "because", "any", "these",
    "give", "day", "most", "us", "is", "are", "was", "were", "been", "has", "had", "said", "did", "made",
    "many", "such", "where", "much", "before", "here", "through", "same", "too", "very", "should", "each",
    "those", "both", "between", "under", "while", "might", "great", "little", "own", "more", "must", "being",
    "off", "down", "still", "world", "life", "become", "around", "another", "part", "every", "found", "thing",
    "always", "something", "without", "however", "again", "against", "never", "during", "really", "although",
    "receive", "believe", "friend", "because", "beautiful", "different", "necessary", "separate", "definitely",
    "brown", "quick", "sentence", "misspelled",
};

int LookupCommonRank(const std::unordered_map<std::string, int> &m, const std::string &lower) {
    auto it = m.find(lower);
    return it == m.end() ? 1000000 : it->second;
}
}  // namespace

void SpellChecker::IndexWord(const std::string &word) {
    if (word.empty()) return;
    int idx = static_cast<int>(words_.size());
    words_.push_back(word);
    lower_set_.insert(ToLowerAscii(word));
    by_len_[static_cast<int>(word.size())].push_back(idx);
}

bool SpellChecker::LoadDictionary(const std::string &path) {
    std::ifstream in(path);
    if (!in) return false;
    words_.clear();
    lower_set_.clear();
    by_len_.clear();
    common_rank_.clear();
    for (int i = 0; i < static_cast<int>(sizeof(kCommonWords) / sizeof(kCommonWords[0])); i++)
        common_rank_.emplace(ToLowerAscii(kCommonWords[i]), i);
    std::string line;
    while (std::getline(in, line)) {
        // Trim trailing CR/whitespace (tolerate CRLF wordlists).
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        IndexWord(line);
    }
    // Re-apply personal-good words to the membership set (LoadDictionary may run
    // after SetPersonalFiles cleared nothing, but a reload must not drop them).
    for (const std::string &w : personal_good_) lower_set_.insert(w);
    return !words_.empty();
}

void SpellChecker::SetPersonalFiles(const std::string &good_path, const std::string &wrong_path) {
    good_path_ = good_path;
    wrong_path_ = wrong_path;
    personal_good_.clear();
    personal_wrong_.clear();
    auto load_into = [](const std::string &p, std::unordered_set<std::string> &set) {
        if (p.empty()) return;
        std::ifstream in(p);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            set.insert(ToLowerAscii(line));
        }
    };
    load_into(good_path_, personal_good_);
    load_into(wrong_path_, personal_wrong_);
    for (const std::string &w : personal_good_) lower_set_.insert(w);
}

bool SpellChecker::IsMisspelled(std::string_view word) const {
    if (!ready()) return false;
    if (word.empty()) return false;
    if (!HasAsciiLetter(word)) return false;  // pure punctuation/symbols
    if (HasDigit(word)) return false;          // identifiers, versions, etc.
    if (ignore_uppercase_ && IsAllUpper(word)) return false;  // acronyms
    std::string lower = ToLowerAscii(word);
    if (personal_wrong_.count(lower)) return true;   // user marked it wrong
    if (personal_good_.count(lower)) return false;   // user marked it good
    return lower_set_.find(lower) == lower_set_.end();
}

std::vector<std::string> SpellChecker::Suggest(std::string_view word, int max) const {
    std::vector<std::string> result;
    if (!ready() || word.empty() || max <= 0) return result;
    std::string lower = ToLowerAscii(word);
    const int wlen = static_cast<int>(lower.size());
    // A word longer than 4 chars can tolerate distance 2; short words only 1,
    // so a two-letter typo doesn't turn into a random unrelated word.
    const int max_dist = wlen <= 4 ? 1 : 2;
    const std::string wkey = Soundex(lower);

    const bool input_has_upper = (lower != std::string(word));  // input carried a capital

    struct Cand {
        int dist;       // edit distance (lower better)
        int common;     // high-frequency rank, 1e6 if not a common word (lower better)
        int lenpen;     // 0 if same length as input (transposition/substitution), else 1
        int casepen;    // 0 if candidate case fits the input, else 1
        int phonetic;   // 0 if Soundex matches, 1 otherwise
        int order;      // wordlist index, final alphabetical tie-break
        int word_idx;
    };
    std::vector<Cand> cands;
    for (int len = wlen - max_dist; len <= wlen + max_dist; ++len) {
        auto it = by_len_.find(len);
        if (it == by_len_.end()) continue;
        for (int idx : it->second) {
            const std::string &cand = words_[static_cast<size_t>(idx)];
            std::string cand_lower = ToLowerAscii(cand);
            if (cand_lower == lower) continue;  // same word, not a suggestion
            int d = EditDistance(lower, cand_lower, max_dist);
            if (d > max_dist) continue;
            int common = LookupCommonRank(common_rank_, cand_lower);
            int lenpen = (static_cast<int>(cand_lower.size()) == wlen) ? 0 : 1;
            // A lowercase input shouldn't be "corrected" to a proper noun /
            // abbreviation (borwn -> Born, teh -> Te): penalize a candidate
            // that carries a capital the input didn't.
            bool cand_has_upper = (cand_lower != cand);
            int casepen = (cand_has_upper && !input_has_upper) ? 1 : 0;
            int ph = (!wkey.empty() && Soundex(cand_lower) == wkey) ? 0 : 1;
            cands.push_back({d, common, lenpen, casepen, ph, idx, idx});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        if (a.common != b.common) return a.common < b.common;
        if (a.lenpen != b.lenpen) return a.lenpen < b.lenpen;
        if (a.casepen != b.casepen) return a.casepen < b.casepen;
        if (a.phonetic != b.phonetic) return a.phonetic < b.phonetic;
        return a.order < b.order;
    });
    for (const Cand &c : cands) {
        if (static_cast<int>(result.size()) >= max) break;
        result.push_back(MatchCase(words_[static_cast<size_t>(c.word_idx)], word));
    }
    return result;
}

void SpellChecker::AddGood(std::string_view word) {
    std::string lower = ToLowerAscii(word);
    if (lower.empty()) return;
    personal_good_.insert(lower);
    personal_wrong_.erase(lower);  // adding as good clears a prior "wrong"
    lower_set_.insert(lower);
    if (good_path_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(good_path_).parent_path(), ec);
    std::ofstream out(good_path_, std::ios::app);
    if (out) out << std::string(word) << '\n';
}

void SpellChecker::AddWrong(std::string_view word) {
    std::string lower = ToLowerAscii(word);
    if (lower.empty()) return;
    personal_wrong_.insert(lower);
    personal_good_.erase(lower);
    if (wrong_path_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(wrong_path_).parent_path(), ec);
    std::ofstream out(wrong_path_, std::ios::app);
    if (out) out << std::string(word) << '\n';
}

}  // namespace spell

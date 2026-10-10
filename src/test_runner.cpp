#include "test_runner.h"

#include <cctype>
#include <cstdlib>

#include "json.h"

namespace meptest {

namespace {

std::string Trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

bool StartsWith(const std::string &s, size_t pos, const char *prefix) {
    return s.compare(pos, std::char_traits<char>::length(prefix), prefix) == 0;
}

// Reads a run of digits at *pos, advancing past it. False if there are none.
bool ReadDigits(const std::string &s, size_t *pos) {
    size_t start = *pos;
    while (*pos < s.size() && std::isdigit(static_cast<unsigned char>(s[*pos]))) (*pos)++;
    return *pos > start;
}

std::string StatusFromDetail(const std::string &detail) {
    if (detail == "Passed") return "passed";
    if (detail.find("Not Run") != std::string::npos) return "notrun";
    if (detail.find("Skipped") != std::string::npos) return "skipped";
    if (detail.find("Timeout") != std::string::npos) return "timeout";
    return "failed";
}

// "    Start 3: name" -> name.
bool ParseStart(const std::string &line, std::string *name) {
    size_t pos = 0;
    while (pos < line.size() && line[pos] == ' ') pos++;
    if (!StartsWith(line, pos, "Start ")) return false;
    pos += 6;
    while (pos < line.size() && line[pos] == ' ') pos++;
    if (!ReadDigits(line, &pos) || !StartsWith(line, pos, ": ")) return false;
    *name = line.substr(pos + 2);
    return !name->empty();
}

// "3/9 Test #3: name ..........***Failed    0.20 sec". The dot leader always
// follows the name after one space (ctest pads with " ." even when a long
// name leaves room for only one dot), the status text runs up to the time,
// and the time is the last field before " sec".
bool ParseResult(const std::string &line, CtestResult *out) {
    size_t pos = 0;
    while (pos < line.size() && line[pos] == ' ') pos++;
    // "N/M " is present in a plain run but absent when ctest re-prints
    // results without a total; tolerate both.
    size_t save = pos;
    if (ReadDigits(line, &pos) && pos < line.size() && line[pos] == '/' && (pos++, ReadDigits(line, &pos)) &&
        pos < line.size() && line[pos] == ' ') {
        while (pos < line.size() && line[pos] == ' ') pos++;
    } else {
        pos = save;
    }
    if (!StartsWith(line, pos, "Test")) return false;
    pos += 4;
    while (pos < line.size() && line[pos] == ' ') pos++;
    if (pos >= line.size() || line[pos] != '#') return false;
    pos++;
    if (!ReadDigits(line, &pos) || !StartsWith(line, pos, ": ")) return false;
    pos += 2;

    size_t leader = line.find(" .", pos);
    if (leader == std::string::npos) return false;
    std::string name = line.substr(pos, leader - pos);
    size_t after = leader + 1;
    while (after < line.size() && line[after] == '.') after++;

    std::string rest = Trim(line.substr(after));
    if (rest.size() < 4 || rest.compare(rest.size() - 4, 4, " sec") != 0) return false;
    rest = Trim(rest.substr(0, rest.size() - 4));
    size_t sp = rest.find_last_of(' ');
    std::string secs = sp == std::string::npos ? rest : rest.substr(sp + 1);
    std::string detail = sp == std::string::npos ? std::string() : Trim(rest.substr(0, sp));
    // ctest marks every non-pass with "***" ("***Failed", "Subprocess
    // aborted***Exception:"); a leading one carries no information.
    if (StartsWith(detail, 0, "***")) detail = detail.substr(3);
    char *end = nullptr;
    double seconds = std::strtod(secs.c_str(), &end);
    if (secs.empty() || end == secs.c_str() || name.empty() || detail.empty()) return false;

    out->name = name;
    out->detail = detail;
    out->status = StatusFromDetail(detail);
    out->seconds = seconds;
    return true;
}

// The lines that end a run's per-test section: the "N% tests passed" summary
// and everything after it.
bool IsSummary(const std::string &line) {
    return line.find("% tests passed") != std::string::npos;
}

}  // namespace

bool ParseCtestList(const std::string &json, std::vector<CtestTest> *out) {
    out->clear();
    Json doc;
    if (!Json::Parse(json, &doc) || !doc.is_object() || doc.get("kind").as_string() != "ctestInfo") return false;
    for (const Json &t : doc.get("tests").items()) {
        CtestTest test;
        test.name = t.get("name").as_string();
        if (test.name.empty()) continue;
        for (const Json &arg : t.get("command").items()) test.command.push_back(arg.as_string());
        for (const Json &prop : t.get("properties").items()) {
            const std::string &pname = prop.get("name").as_string();
            if (pname == "WORKING_DIRECTORY") {
                test.working_dir = prop.get("value").as_string();
            } else if (pname == "LABELS") {
                for (const Json &label : prop.get("value").items()) test.labels.push_back(label.as_string());
            }
        }
        out->push_back(std::move(test));
    }
    return true;
}

CtestLine ParseCtestLine(const std::string &line) {
    CtestLine out;
    if (ParseStart(line, &out.result.name)) {
        out.kind = CtestLine::Kind::Start;
    } else if (ParseResult(line, &out.result)) {
        out.kind = CtestLine::Kind::Result;
    } else {
        out.result = CtestResult();
    }
    return out;
}

std::vector<CtestResult> ParseCtestOutput(const std::vector<std::string> &lines) {
    std::vector<CtestResult> results;
    // Output can land on either side of a test's Result line: its own
    // stdout/stderr (--output-on-failure) comes after, but ctest's reason
    // for a "Not Run" ("Could not find executable ...") comes between its
    // Start and Result lines. So lines after a Start are held until that
    // test's Result claims them; lines after a Result belong to it.
    std::vector<std::string> pending;
    bool after_result = false;
    auto trim_tail = [](std::vector<std::string> &o) {
        while (!o.empty() && Trim(o.back()).empty()) o.pop_back();
    };
    for (const std::string &line : lines) {
        if (IsSummary(line)) break;
        CtestLine parsed = ParseCtestLine(line);
        if (parsed.kind == CtestLine::Kind::Result) {
            if (after_result) trim_tail(results.back().output);
            trim_tail(pending);
            parsed.result.output = std::move(pending);
            pending.clear();
            results.push_back(std::move(parsed.result));
            after_result = true;
        } else if (parsed.kind == CtestLine::Kind::Start) {
            if (after_result) trim_tail(results.back().output);
            pending.clear();
            after_result = false;
        } else if (after_result) {
            results.back().output.push_back(line);
        } else if (!pending.empty() || !Trim(line).empty()) {
            pending.push_back(line);
        }
    }
    if (after_result) trim_tail(results.back().output);
    return results;
}

std::string CtestExactNameRegex(const std::string &name) {
    std::string out = "^";
    for (char c : name) {
        if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos) out += '\\';
        out += c;
    }
    out += '$';
    return out;
}

std::string CtestBuildTarget(const CtestTest &test) {
    if (test.command.empty() || test.command[0].empty()) return test.name;
    const std::string &exe = test.command[0];
    size_t slash = exe.find_last_of("/\\");
    std::string base = slash == std::string::npos ? exe : exe.substr(slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".exe") == 0) base.resize(base.size() - 4);
    return base.empty() ? test.name : base;
}

}  // namespace meptest

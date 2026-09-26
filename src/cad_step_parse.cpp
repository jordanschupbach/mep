// The ISO 10303-21 physical file, and nothing about geometry.
//
// The grammar is small and completely regular, which is the one genuinely
// pleasant thing about STEP: a file is a header section and a data
// section, a section is a list of entity instances, an instance is an id,
// a type name and a parenthesised argument list, and an argument is one
// of nine things. All of that is below and all of it is handled. What is
// hard about STEP is entirely in the layer above.

#include "cad_step.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cad {
namespace {

struct Scanner {
    const std::string &text;
    std::size_t pos = 0;
    int line = 1;
    std::string error;

    explicit Scanner(const std::string &source) : text(source) {}

    bool Fail(const std::string &message) {
        if (error.empty()) error = "line " + std::to_string(line) + ": " + message;
        return false;
    }
    bool Done() const { return pos >= text.size(); }
    char Peek() const { return pos < text.size() ? text[pos] : '\0'; }
    char Take() {
        const char c = text[pos++];
        if (c == '\n') ++line;
        return c;
    }

    // Whitespace and comments. A Part 21 comment is /* ... */ and may
    // span lines; there is no line comment, which is why an unterminated
    // one swallows the rest of the file and is worth reporting rather
    // than running off the end.
    bool Skip() {
        for (;;) {
            while (!Done() && (Peek() == ' ' || Peek() == '\t' || Peek() == '\r' || Peek() == '\n')) {
                Take();
            }
            if (pos + 1 < text.size() && text[pos] == '/' && text[pos + 1] == '*') {
                const int started = line;
                pos += 2;
                bool closed = false;
                while (pos + 1 < text.size()) {
                    if (text[pos] == '*' && text[pos + 1] == '/') {
                        pos += 2;
                        closed = true;
                        break;
                    }
                    Take();
                }
                if (!closed) return Fail("a comment opened on line " + std::to_string(started) +
                                         " is never closed");
                continue;
            }
            return true;
        }
    }

    bool Expect(char c) {
        if (!Skip()) return false;
        if (Peek() != c) {
            return Fail(std::string("expected '") + c + "' but found '" +
                        (Done() ? std::string("end of file") : std::string(1, Peek())) + "'");
        }
        Take();
        return true;
    }

    // A keyword: a type name, a section name, or the tail of an
    // enumeration. Part 21 allows letters, digits, underscores, and a
    // leading ! for entities outside the schema.
    std::string Keyword() {
        std::string out;
        if (!Skip()) return out;
        if (Peek() == '!') out.push_back(Take());
        while (!Done()) {
            const char c = Peek();
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
                out.push_back(Take());
            } else {
                break;
            }
        }
        return out;
    }
};

bool ParseValue(Scanner *s, StepValue *out);

// A string literal. Part 21 escapes a quote by doubling it, and encodes
// non-ASCII with \X2\....\X0\ and friends. The doubling is handled here;
// the encodings are left as written, because turning them into UTF-8 is
// a presentation decision and the round trip has to be exact.
bool ParseString(Scanner *s, StepValue *out) {
    if (!s->Expect('\'')) return false;
    out->kind = StepValue::Kind::String;
    out->text.clear();
    for (;;) {
        if (s->Done()) return s->Fail("a string literal is never closed");
        const char c = s->Take();
        if (c != '\'') {
            out->text.push_back(c);
            continue;
        }
        if (s->Peek() == '\'') {
            s->Take();
            out->text.push_back('\'');
            continue;
        }
        return true;
    }
}

bool ParseNumber(Scanner *s, StepValue *out) {
    const std::size_t start = s->pos;
    if (s->Peek() == '+' || s->Peek() == '-') s->Take();
    bool any = false;
    while (!s->Done() && std::isdigit(static_cast<unsigned char>(s->Peek())) != 0) {
        s->Take();
        any = true;
    }
    bool real = false;
    if (s->Peek() == '.') {
        real = true;
        s->Take();
        while (!s->Done() && std::isdigit(static_cast<unsigned char>(s->Peek())) != 0) {
            s->Take();
            any = true;
        }
    }
    if (s->Peek() == 'E' || s->Peek() == 'e') {
        real = true;
        s->Take();
        if (s->Peek() == '+' || s->Peek() == '-') s->Take();
        while (!s->Done() && std::isdigit(static_cast<unsigned char>(s->Peek())) != 0) s->Take();
    }
    if (!any) return s->Fail("expected a number");
    const std::string token = s->text.substr(start, s->pos - start);
    if (real) {
        out->kind = StepValue::Kind::Real;
        out->real = std::strtod(token.c_str(), nullptr);
    } else {
        out->kind = StepValue::Kind::Integer;
        out->integer = std::strtoll(token.c_str(), nullptr, 10);
        out->real = static_cast<double>(out->integer);
    }
    return true;
}

bool ParseList(Scanner *s, std::vector<StepValue> *out) {
    if (!s->Expect('(')) return false;
    if (!s->Skip()) return false;
    if (s->Peek() == ')') {
        s->Take();
        return true;
    }
    for (;;) {
        StepValue value;
        if (!ParseValue(s, &value)) return false;
        out->push_back(std::move(value));
        if (!s->Skip()) return false;
        if (s->Peek() == ',') {
            s->Take();
            continue;
        }
        return s->Expect(')');
    }
}

bool ParseValue(Scanner *s, StepValue *out) {
    if (!s->Skip()) return false;
    if (s->Done()) return s->Fail("the file ends in the middle of a value");
    const char c = s->Peek();
    if (c == '$') {
        s->Take();
        out->kind = StepValue::Kind::Unset;
        return true;
    }
    if (c == '*') {
        s->Take();
        out->kind = StepValue::Kind::Derived;
        return true;
    }
    if (c == '#') {
        s->Take();
        StepValue number;
        if (!ParseNumber(s, &number)) return false;
        out->kind = StepValue::Kind::Reference;
        out->reference = static_cast<int>(number.integer);
        return true;
    }
    if (c == '\'') return ParseString(s, out);
    if (c == '"') {
        s->Take();
        out->kind = StepValue::Kind::Binary;
        while (!s->Done() && s->Peek() != '"') out->text.push_back(s->Take());
        return s->Expect('"');
    }
    if (c == '.') {
        s->Take();
        out->kind = StepValue::Kind::Enumeration;
        out->text = s->Keyword();
        return s->Expect('.');
    }
    if (c == '(') {
        out->kind = StepValue::Kind::List;
        return ParseList(s, &out->items);
    }
    if (c == '+' || c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
        return ParseNumber(s, out);
    }
    if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '!' || c == '_') {
        out->kind = StepValue::Kind::Typed;
        out->text = s->Keyword();
        return ParseList(s, &out->items);
    }
    return s->Fail(std::string("'") + c + "' does not start any kind of value");
}

// One instance: `#12 = TYPE(args);` or the complex form
// `#12 = (A(args) B(args));`.
bool ParseInstance(Scanner *s, StepEntity *out) {
    if (!s->Skip()) return false;
    if (!s->Expect('#')) return false;
    StepValue id;
    if (!ParseNumber(s, &id)) return false;
    out->id = static_cast<int>(id.integer);
    out->line = s->line;
    if (!s->Expect('=')) return false;
    if (!s->Skip()) return false;
    if (s->Peek() == '(') {
        // Complex: a parenthesised run of simple records.
        s->Take();
        for (;;) {
            if (!s->Skip()) return false;
            if (s->Peek() == ')') {
                s->Take();
                break;
            }
            StepEntity part;
            part.id = out->id;
            part.line = s->line;
            part.type = s->Keyword();
            if (part.type.empty()) return s->Fail("expected a type name inside a complex instance");
            if (!ParseList(s, &part.arguments)) return false;
            out->parts.push_back(std::move(part));
        }
        if (!out->parts.empty()) out->type = out->parts.front().type;
    } else {
        out->type = s->Keyword();
        if (out->type.empty()) return s->Fail("expected a type name after '='");
        if (!ParseList(s, &out->arguments)) return false;
    }
    return s->Expect(';');
}

}  // namespace

const StepEntity *StepFile::Get(int id) const {
    const auto found = entities.find(id);
    return found == entities.end() ? nullptr : &found->second;
}

std::vector<const StepEntity *> StepFile::OfType(const std::string &type) const {
    std::vector<const StepEntity *> out;
    for (const auto &entry : entities) {
        if (entry.second.type == type) {
            out.push_back(&entry.second);
            continue;
        }
        for (const StepEntity &part : entry.second.parts) {
            if (part.type == type) {
                out.push_back(&entry.second);
                break;
            }
        }
    }
    return out;
}

std::string StepFile::SchemaName() const {
    for (const StepEntity &entity : header) {
        if (entity.type != "FILE_SCHEMA") continue;
        if (entity.arguments.empty() || entity.arguments[0].items.empty()) continue;
        return entity.arguments[0].items[0].text;
    }
    return "";
}

bool ParseStepFile(const std::string &text, StepFile *out, std::string *error) {
    error->clear();
    *out = StepFile{};
    Scanner s(text);
    if (!s.Skip()) {
        *error = s.error;
        return false;
    }
    // ISO-10303-21; ... END-ISO-10303-21;
    const std::string opening = s.Keyword();
    if (opening != "ISO" ) {
        *error = "this does not begin with ISO-10303-21, so it is not a STEP physical file";
        return false;
    }
    // The tag is ISO-10303-21, which the keyword scanner stops at the
    // hyphen of; skip to the semicolon rather than re-implementing it.
    while (!s.Done() && s.Peek() != ';') s.Take();
    if (!s.Expect(';')) {
        *error = s.error;
        return false;
    }

    bool seen_data = false;
    for (;;) {
        if (!s.Skip()) {
            *error = s.error;
            return false;
        }
        if (s.Done()) break;
        if (s.Peek() == '#') {
            StepEntity entity;
            if (!ParseInstance(&s, &entity)) {
                *error = s.error;
                return false;
            }
            if (out->entities.count(entity.id) != 0) {
                *error = "line " + std::to_string(entity.line) + ": #" + std::to_string(entity.id) +
                         " is defined twice";
                return false;
            }
            out->entities.emplace(entity.id, std::move(entity));
            continue;
        }
        const std::string word = s.Keyword();
        if (word == "HEADER") {
            if (!s.Expect(';')) {
                *error = s.error;
                return false;
            }
            // The header's entities have no ids.
            for (;;) {
                if (!s.Skip()) {
                    *error = s.error;
                    return false;
                }
                const std::size_t mark = s.pos;
                const int mark_line = s.line;
                const std::string name = s.Keyword();
                if (name == "ENDSEC") {
                    if (!s.Expect(';')) {
                        *error = s.error;
                        return false;
                    }
                    break;
                }
                if (name.empty()) {
                    *error = "line " + std::to_string(mark_line) + ": the header section is malformed";
                    return false;
                }
                s.pos = mark;
                s.line = mark_line;
                StepEntity entity;
                entity.line = s.line;
                entity.type = s.Keyword();
                if (!ParseList(&s, &entity.arguments) || !s.Expect(';')) {
                    *error = s.error;
                    return false;
                }
                out->header.push_back(std::move(entity));
            }
            continue;
        }
        if (word == "DATA") {
            seen_data = true;
            // DATA may carry a parameter list: DATA('name');
            if (!s.Skip()) {
                *error = s.error;
                return false;
            }
            if (s.Peek() == '(') {
                std::vector<StepValue> ignored;
                if (!ParseList(&s, &ignored)) {
                    *error = s.error;
                    return false;
                }
            }
            if (!s.Expect(';')) {
                *error = s.error;
                return false;
            }
            continue;
        }
        if (word == "ENDSEC") {
            if (!s.Expect(';')) {
                *error = s.error;
                return false;
            }
            continue;
        }
        if (word == "END") {
            // END-ISO-10303-21;
            while (!s.Done() && s.Peek() != ';') s.Take();
            if (!s.Done()) s.Take();
            break;
        }
        if (word.empty()) {
            *error = "line " + std::to_string(s.line) + ": unexpected character '" +
                     std::string(1, s.Peek()) + "'";
            return false;
        }
        *error = "line " + std::to_string(s.line) + ": unexpected section '" + word + "'";
        return false;
    }
    if (!seen_data) {
        *error = "the file has no DATA section";
        return false;
    }
    return true;
}

// --- Writing back out ----------------------------------------------------

namespace {

void WriteValue(const StepValue &value, std::string *out);

// STEP reals must always carry a '.', and integers must never. Getting
// that wrong produces a file that many readers reject outright, so the
// formatting is done here rather than with a bare printf anywhere it is
// needed.
std::string FormatReal(double value) {
    if (!std::isfinite(value)) return "0.";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17G", value);
    std::string text(buffer);
    if (text.find('.') == std::string::npos && text.find('E') == std::string::npos &&
        text.find("INF") == std::string::npos && text.find("NAN") == std::string::npos) {
        text += ".";
    }
    // 1E-5 is not legal; it has to be 1.E-5.
    const std::size_t e = text.find('E');
    if (e != std::string::npos && text.find('.') == std::string::npos) {
        text.insert(e, ".");
    }
    return text;
}

std::string QuoteString(const std::string &text) {
    std::string out = "'";
    for (char c : text) {
        out.push_back(c);
        if (c == '\'') out.push_back('\'');
    }
    out.push_back('\'');
    return out;
}

void WriteList(const std::vector<StepValue> &items, std::string *out) {
    out->push_back('(');
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out->push_back(',');
        WriteValue(items[i], out);
    }
    out->push_back(')');
}

void WriteValue(const StepValue &value, std::string *out) {
    switch (value.kind) {
        case StepValue::Kind::Unset: out->push_back('$'); break;
        case StepValue::Kind::Derived: out->push_back('*'); break;
        case StepValue::Kind::Integer: *out += std::to_string(value.integer); break;
        case StepValue::Kind::Real: *out += FormatReal(value.real); break;
        case StepValue::Kind::String: *out += QuoteString(value.text); break;
        case StepValue::Kind::Enumeration: *out += "." + value.text + "."; break;
        case StepValue::Kind::Binary: *out += "\"" + value.text + "\""; break;
        case StepValue::Kind::Reference: *out += "#" + std::to_string(value.reference); break;
        case StepValue::Kind::List: WriteList(value.items, out); break;
        case StepValue::Kind::Typed:
            *out += value.text;
            WriteList(value.items, out);
            break;
    }
}

void WriteEntityBody(const StepEntity &entity, std::string *out) {
    if (entity.IsComplex()) {
        out->push_back('(');
        for (const StepEntity &part : entity.parts) {
            *out += part.type;
            WriteList(part.arguments, out);
        }
        out->push_back(')');
        return;
    }
    *out += entity.type;
    WriteList(entity.arguments, out);
}

}  // namespace

std::string WriteStepFile(const StepFile &file, const std::string &schema) {
    std::string out = "ISO-10303-21;\nHEADER;\n";
    bool wrote_schema = false;
    for (const StepEntity &entity : file.header) {
        WriteEntityBody(entity, &out);
        out += ";\n";
        if (entity.type == "FILE_SCHEMA") wrote_schema = true;
    }
    if (!wrote_schema && !schema.empty()) {
        out += "FILE_SCHEMA((" + QuoteString(schema) + "));\n";
    }
    out += "ENDSEC;\nDATA;\n";
    for (const auto &entry : file.entities) {
        out += "#" + std::to_string(entry.first) + "=";
        WriteEntityBody(entry.second, &out);
        out += ";\n";
    }
    out += "ENDSEC;\nEND-ISO-10303-21;\n";
    return out;
}

}  // namespace cad

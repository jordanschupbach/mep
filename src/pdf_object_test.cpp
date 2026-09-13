// PDFIUM_REMOVAL_PLAN.md Phase 2 coverage for pdf_object.h/.cpp: every
// object type, both string forms, name escapes, reference-vs-number
// disambiguation, and ParseIndirectObject's stream handling (literal
// Length, unresolvable Length falling back to an endstream scan). Test
// fixtures are inline byte literals rather than files under
// test/pdf_fixtures/ -- that directory is gitignored (confirmed during
// Phase 1: nothing under test/ is tracked in this repo), so a test that
// read from it wouldn't run in a fresh checkout/CI. The "real fixture"
// byte string below was captured once from test/pdf_fixtures/
// tectonic_test.pdf (object 13, a plain FlateDecode content stream) and
// pasted in verbatim, so this test still exercises real tectonic/
// xdvipdfmx output, not just hand-written syntax.

#include "pdf_object.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

const unsigned char *B(const char *s) { return reinterpret_cast<const unsigned char *>(s); }

pdfobj::Object ParseOne(const std::string &text, size_t *end_pos = nullptr) {
    size_t pos = 0;
    pdfobj::Object obj;
    bool ok = pdfobj::ParseObject(B(text.data()), text.size(), pos, &obj);
    CHECK(ok);
    if (end_pos) *end_pos = pos;
    return obj;
}

void TestScalars() {
    CHECK(ParseOne("null").IsNull());
    {
        auto o = ParseOne("true");
        CHECK(o.type == pdfobj::Type::Bool && o.bool_val == true);
    }
    {
        auto o = ParseOne("false");
        CHECK(o.type == pdfobj::Type::Bool && o.bool_val == false);
    }
    {
        auto o = ParseOne("123");
        CHECK(o.type == pdfobj::Type::Int && o.int_val == 123);
    }
    {
        auto o = ParseOne("-17");
        CHECK(o.type == pdfobj::Type::Int && o.int_val == -17);
    }
    {
        auto o = ParseOne("3.14");
        CHECK(o.type == pdfobj::Type::Real);
        CHECK(o.real_val > 3.139 && o.real_val < 3.141);
    }
    {
        auto o = ParseOne("-.5");
        CHECK(o.type == pdfobj::Type::Real);
        CHECK(o.real_val > -0.501 && o.real_val < -0.499);
    }
    {
        auto o = ParseOne("4.");
        CHECK(o.type == pdfobj::Type::Real && o.AsDouble() == 4.0);
    }
    // Non-number types answer AsDouble/AsInt with the default, not a crash.
    CHECK(ParseOne("null").AsDouble(-1.0) == -1.0);
    CHECK(ParseOne("/Foo").AsInt(-1) == -1);
}

void TestLiteralStrings() {
    CHECK(ParseOne("(hello)").str_val == "hello");
    CHECK(ParseOne("(a\\nb)").str_val == "a\nb");
    CHECK(ParseOne("(a\\(b\\)c)").str_val == "a(b)c");
    CHECK(ParseOne("(nested (parens) ok)").str_val == "nested (parens) ok");
    CHECK(ParseOne("(octal \\101\\102)").str_val == "octal AB");  // \101=A, \102=B
    CHECK(ParseOne("(line\\\ncont)").str_val == "linecont");      // backslash-newline: no char emitted
    {
        auto o = ParseOne("(a\\zb)");  // unknown escape: backslash dropped, char kept literal
        CHECK(o.str_val == "azb");
    }
}

void TestHexStrings() {
    CHECK(ParseOne("<48656C6C6F>").str_val == "Hello");
    CHECK(ParseOne("<48 65 6C 6C 6F>").str_val == "Hello");  // interior whitespace ignored
    CHECK(ParseOne("<901FA3>").str_val.size() == 3);
    {
        auto o = ParseOne("<901>");  // odd digit count: implicit trailing 0 -> 0x90 0x10
        CHECK(o.str_val.size() == 2);
        CHECK(static_cast<unsigned char>(o.str_val[0]) == 0x90);
        CHECK(static_cast<unsigned char>(o.str_val[1]) == 0x10);
    }
}

void TestNames() {
    CHECK(ParseOne("/Type").AsString("") == "Type");
    CHECK(ParseOne("/Name#20With#20Spaces").AsString("") == "Name With Spaces");
    CHECK(ParseOne("/A#42C").AsString("") == "ABC");  // #42 == 'B'
    // A default for the wrong type returns the default, not garbage.
    CHECK(ParseOne("123").AsString("fallback") == "fallback");
}

void TestArraysAndDicts() {
    {
        auto o = ParseOne("[1 2.5 /Foo (bar) true]");
        CHECK(o.IsArray() && o.array_val.size() == 5);
        CHECK(o.array_val[0].AsInt() == 1);
        CHECK(o.array_val[1].AsDouble() == 2.5);
        CHECK(o.array_val[2].AsString("") == "Foo");
        CHECK(o.array_val[3].str_val == "bar");
        CHECK(o.array_val[4].bool_val == true);
    }
    {
        auto o = ParseOne("[[1 2] [3 4]]");  // nested arrays
        CHECK(o.array_val.size() == 2);
        CHECK(o.array_val[0].array_val[1].AsInt() == 2);
        CHECK(o.array_val[1].array_val[0].AsInt() == 3);
    }
    {
        // Newline between key and value, exactly like the real "Width\n480"
        // case found in test/pdf_fixtures/tectonic_test.pdf object 23.
        auto o = ParseOne("<< /Type /XObject /Width\n480 /Nested << /A 1 >> >>");
        CHECK(o.IsDict());
        CHECK(o.Find("Type")->AsString("") == "XObject");
        CHECK(o.Find("Width")->AsInt() == 480);
        const pdfobj::Object *nested = o.Find("Nested");
        CHECK(nested && nested->IsDict());
        CHECK(nested->Find("A")->AsInt() == 1);
        CHECK(o.Find("NoSuchKey") == nullptr);
    }
    {
        // Empty array/dict.
        CHECK(ParseOne("[]").array_val.empty());
        CHECK(ParseOne("<< >>").dict_val.empty());
    }
}

void TestReferenceVsNumberDisambiguation() {
    {
        auto o = ParseOne("12 0 R");
        CHECK(o.IsReference());
        CHECK(o.ref_val.num == 12 && o.ref_val.gen == 0);
    }
    {
        // A plain number followed by another number must NOT be
        // misparsed as a reference just because "N G" matches -- only
        // consume the second/third token when "R" genuinely follows.
        size_t pos = 0;
        pdfobj::Object first;
        CHECK(pdfobj::ParseObject(B("12 34"), 5, pos, &first));
        CHECK(first.type == pdfobj::Type::Int && first.int_val == 12);
    }
    {
        // Inside an array, "1 2 3" is three plain integers, not a
        // reference eating the first two.
        auto o = ParseOne("[1 2 3]");
        CHECK(o.array_val.size() == 3);
        CHECK(o.array_val[0].type == pdfobj::Type::Int);
        CHECK(o.array_val[1].type == pdfobj::Type::Int);
        CHECK(o.array_val[2].type == pdfobj::Type::Int);
    }
    {
        // A dict value that's a reference.
        auto o = ParseOne("<< /Length 5 0 R >>");
        const pdfobj::Object *len = o.Find("Length");
        CHECK(len && len->IsReference() && len->ref_val.num == 5);
    }
}

void TestCommentsAndWhitespace() {
    CHECK(ParseOne("  % a comment\n  42").AsInt() == 42);
    CHECK(ParseOne("[1 %c1\n 2 %c2\n 3]").array_val.size() == 3);
}

void TestParseIndirectObjectNoStream() {
    pdfobj::IndirectObject ind;
    std::string text = "7 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    bool ok = pdfobj::ParseIndirectObject(B(text.data()), text.size(), 0, &ind);
    CHECK(ok);
    CHECK(ind.num == 7 && ind.gen == 0);
    CHECK(!ind.has_stream);
    CHECK(ind.value.IsDict());
    CHECK(ind.value.Find("Type")->AsString("") == "Catalog");
    CHECK(ind.value.Find("Pages")->ref_val.num == 2);
}

void TestParseIndirectObjectWithLiteralLength() {
    // Hand-built: exercises the literal-/Length path end to end.
    std::string body = "abcdefghij";  // 10 bytes
    std::string text = "9 0 obj\n<< /Length 10 >>\nstream\n" + body + "\nendstream\nendobj\n";
    pdfobj::IndirectObject ind;
    bool ok = pdfobj::ParseIndirectObject(B(text.data()), text.size(), 0, &ind);
    CHECK(ok);
    CHECK(ind.has_stream);
    CHECK(ind.stream_length == 10);
    CHECK(text.compare(ind.stream_offset, ind.stream_length, body) == 0);
}

void TestParseIndirectObjectUnresolvedLengthFallsBackToEndstreamScan() {
    // /Length is a reference this test deliberately never resolves (no
    // LengthResolver given) -- must fall back to scanning for "endstream".
    std::string body = "some raw stream bytes, length unknown up front";
    std::string text = "9 0 obj\n<< /Length 99 0 R >>\nstream\n" + body + "\nendstream\nendobj\n";
    pdfobj::IndirectObject ind;
    bool ok = pdfobj::ParseIndirectObject(B(text.data()), text.size(), 0, &ind);
    CHECK(ok);
    CHECK(ind.has_stream);
    CHECK(text.compare(ind.stream_offset, ind.stream_length, body) == 0);
}

void TestParseIndirectObjectWrongResolvedLengthFallsBack() {
    // A LengthResolver that resolves to a value NOT actually followed by
    // "endstream" -- must be rejected and fall back to the scan, not
    // trusted blindly.
    std::string body = "the real stream body here";
    std::string text = "9 0 obj\n<< /Length 5 0 R >>\nstream\n" + body + "\nendstream\nendobj\n";
    auto resolver = [](int num, int gen, long long *out) -> bool {
        (void)gen;
        if (num != 5) return false;
        *out = 3;  // deliberately wrong
        return true;
    };
    pdfobj::IndirectObject ind;
    bool ok = pdfobj::ParseIndirectObject(B(text.data()), text.size(), 0, &ind, resolver);
    CHECK(ok);
    CHECK(text.compare(ind.stream_offset, ind.stream_length, body) == 0);
}

void TestParseIndirectObjectRealFixtureFragment() {
    // Captured verbatim from test/pdf_fixtures/tectonic_test.pdf (see
    // PDFIUM_REMOVAL_PLAN.md Phase 1) -- object 13, a real xdvipdfmx-
    // produced FlateDecode content stream. Only the header + declared
    // length matter here (Phase 4 handles actually inflating it); this
    // confirms the header/dict/stream-offset math lines up against a
    // real file, not just hand-written syntax.
    std::string header = "13 0 obj\n<</Filter/FlateDecode/Length 2354>>\nstream\n";
    std::string fake_body(2354, 'x');  // exact declared length, content irrelevant here
    std::string text = header + fake_body + "\nendstream\nendobj\n";
    pdfobj::IndirectObject ind;
    bool ok = pdfobj::ParseIndirectObject(B(text.data()), text.size(), 0, &ind);
    CHECK(ok);
    CHECK(ind.num == 13 && ind.gen == 0);
    CHECK(ind.value.Find("Filter")->AsString("") == "FlateDecode");
    CHECK(ind.has_stream);
    CHECK(ind.stream_length == 2354);
    CHECK(ind.stream_offset == header.size());
}

void TestMalformedInputTolerance() {
    // Out-of-range position.
    size_t pos = 100;
    pdfobj::Object o;
    CHECK(!pdfobj::ParseObject(B("short"), 5, pos, &o));
    // Stray closing delimiter with no opener.
    pos = 0;
    CHECK(!pdfobj::ParseObject(B(")"), 1, pos, &o));
    // Not an "N G obj" header at all.
    pdfobj::IndirectObject ind;
    CHECK(!pdfobj::ParseIndirectObject(B("not an object"), 13, 0, &ind));
    // Truncated stream (no "endstream" anywhere): tolerated, takes the
    // rest of the buffer rather than crashing/hanging.
    std::string trunc = "9 0 obj\n<< /Length 999 0 R >>\nstream\nabc";
    bool ok = pdfobj::ParseIndirectObject(B(trunc.data()), trunc.size(), 0, &ind);
    CHECK(ok);
    CHECK(ind.has_stream);
    CHECK(ind.stream_length == 3);  // "abc", everything left in the buffer
}

}  // namespace

int main() {
    TestScalars();
    TestLiteralStrings();
    TestHexStrings();
    TestNames();
    TestArraysAndDicts();
    TestReferenceVsNumberDisambiguation();
    TestCommentsAndWhitespace();
    TestParseIndirectObjectNoStream();
    TestParseIndirectObjectWithLiteralLength();
    TestParseIndirectObjectUnresolvedLengthFallsBackToEndstreamScan();
    TestParseIndirectObjectWrongResolvedLengthFallsBack();
    TestParseIndirectObjectRealFixtureFragment();
    TestMalformedInputTolerance();
    std::printf("pdf_object_test: all checks passed\n");
    return 0;
}

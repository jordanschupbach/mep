// Windowless test for rust_lsp_vocab.cpp -- the generated Rust standard
// library vocabulary behind mep-rust-lsp. CHECK(), never assert(): the
// Release build strips assert().
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "rust_lsp_vocab.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}

#define CHECK(c) Check((c), #c, __LINE__)

bool Contains(const char *haystack, std::string_view needle) {
    return haystack != nullptr && std::string_view(haystack).find(needle) != std::string_view::npos;
}

bool Is(const RustVocabEntry *e, RustVocabKind kind) { return e != nullptr && e->kind == kind; }

bool HasMember(const std::vector<RustVocabMemberRef> &v, std::string_view name, std::string_view via_trait) {
    for (const RustVocabMemberRef &m : v) {
        if (m.entry->name != name) continue;
        if (via_trait.empty() ? m.via == nullptr : (m.via != nullptr && m.via->trait == via_trait)) return true;
    }
    return false;
}

void TestTable() {
    CHECK(RustVocabSize() > 4000);
    // Sorted by path, then name: binary search depends on it.
    for (size_t i = 1; i < RustVocabSize(); ++i) {
        const RustVocabEntry &a = RustVocabAt(i - 1);
        const RustVocabEntry &b = RustVocabAt(i);
        const int c = std::string_view(a.path).compare(b.path);
        CHECK(c < 0 || (c == 0 && std::string_view(a.name) <= std::string_view(b.name)));
    }
}

void TestSpotChecks() {
    const RustVocabEntry *e = RustVocabFind("std::vec::Vec::push");
    CHECK(Is(e, RustVocabKind::Method));
    CHECK(Contains(e->signature, "pub fn push(&mut self, value: T)"));
    CHECK(Contains(e->doc, "Appends an element"));
    CHECK(std::string_view(e->since) == "1.0.0");

    e = RustVocabFindMember("std::vec::Vec", "with_capacity");
    CHECK(Is(e, RustVocabKind::AssocFn));
    CHECK(Contains(e->signature, "with_capacity(capacity: usize) -> Self"));

    e = RustVocabFind("std::option::Option::map");
    CHECK(Is(e, RustVocabKind::Method));
    CHECK(Contains(e->signature, "where F: FnOnce(T) -> U"));

    e = RustVocabFind("std::result::Result::unwrap_or_else");
    CHECK(Is(e, RustVocabKind::Method));

    e = RustVocabFind("i32::checked_add");
    CHECK(Is(e, RustVocabKind::Method));
    CHECK(Contains(e->signature, "checked_add(self, rhs: Self) -> Option<Self>"));
    CHECK(Is(RustVocabFind("u8::is_ascii_digit"), RustVocabKind::Method));
    CHECK(Is(RustVocabFind("str::split_whitespace"), RustVocabKind::Method));
    CHECK(Is(RustVocabFind("std::primitive::i32"), RustVocabKind::Primitive));
    CHECK(Is(RustVocabFind("i32::MAX"), RustVocabKind::AssocConst));

    e = RustVocabFind("std::string::String::from_utf8");
    CHECK(Is(e, RustVocabKind::AssocFn));
    CHECK(Contains(e->signature, "Result<String, FromUtf8Error>"));

    e = RustVocabFind("std::collections::HashMap::entry");
    CHECK(Is(e, RustVocabKind::Method));
    CHECK(Is(RustVocabFind("std::fs::read_to_string"), RustVocabKind::Fn));
    CHECK(Is(RustVocabFind("std::io::Write::write_all"), RustVocabKind::Method));
    CHECK(Is(RustVocabFind("std::iter::Iterator::filter_map"), RustVocabKind::Method));
    CHECK(Is(RustVocabFind("std::thread::spawn"), RustVocabKind::Fn));
    CHECK(Contains(RustVocabFind("std::thread::spawn")->signature, "F: FnOnce() -> T + Send + 'static"));

    // Arc::clone comes from Clone, through an impl row.
    CHECK(RustVocabImplements("std::sync::Arc", "std::clone::Clone"));
    CHECK(HasMember(RustVocabTypeMembers("std::sync::Arc"), "clone", "std::clone::Clone"));

    // Macros, keywords, attributes.
    e = RustVocabMacro("println");
    CHECK(Is(e, RustVocabKind::Macro));
    CHECK(std::string_view(e->path) == "std::println");
    CHECK(Contains(e->signature, "macro_rules! println"));
    CHECK(e->in_prelude);
    CHECK(Is(RustVocabMacro("vec!"), RustVocabKind::Macro));
    CHECK(Contains(RustVocabMacro("matches")->signature, "$pattern:pat"));
    CHECK(Is(RustVocabMacro("assert_eq"), RustVocabKind::Macro));
    CHECK(Is(RustVocabMacro("format"), RustVocabKind::Macro));
    CHECK(RustVocabMacros().size() > 30);

    e = RustVocabKeyword("match");
    CHECK(Is(e, RustVocabKind::Keyword));
    CHECK(Contains(e->doc, "pattern"));
    CHECK(Is(RustVocabKeyword("Self"), RustVocabKind::Keyword));
    CHECK(Is(RustVocabKeyword("yield"), RustVocabKind::Keyword));
    CHECK(std::string_view(RustVocabKeyword("async")->since) == "2018");
    CHECK(RustVocabKeyword("matchx") == nullptr);

    e = RustVocabAttribute("derive");
    CHECK(Is(e, RustVocabKind::Attribute));
    CHECK(Contains(e->signature, "#[derive("));
    CHECK(Is(RustVocabAttribute("inline"), RustVocabKind::Attribute));
    CHECK(Is(RustVocabAttribute("cfg"), RustVocabKind::Attribute));
    CHECK(Is(RustVocabAttribute("test"), RustVocabKind::Attribute));
}

void TestPaths() {
    // core:: / alloc:: / re-export paths resolve to the canonical std one.
    CHECK(RustVocabCanonicalPath("core::option::Option") == "std::option::Option");
    CHECK(RustVocabCanonicalPath("alloc::vec::Vec") == "std::vec::Vec");
    CHECK(RustVocabCanonicalPath("std::prelude::v1::Vec") == "std::vec::Vec");
    CHECK(RustVocabCanonicalPath("std::collections::hash_map::HashMap") == "std::collections::HashMap");
    CHECK(RustVocabCanonicalPath("::std::vec::Vec") == "std::vec::Vec");
    CHECK(RustVocabCanonicalPath("std::prelude::v1::Some") == "std::option::Option::Some");
    CHECK(RustVocabCanonicalPath("no::such::thing").empty());
    CHECK(Is(RustVocabFind("core::option::Option::Some"), RustVocabKind::Variant));
    CHECK(Is(RustVocabFind("alloc::string::String::push_str"), RustVocabKind::Method));
    CHECK(Is(RustVocabFind("core::iter::Iterator"), RustVocabKind::Trait));
    CHECK(RustVocabFind("std::vec::Vec::no_such_method") == nullptr);
    CHECK(RustVocabFind("") == nullptr);

    // std::vec is a module and a macro; Find prefers the module, FindAll has both.
    CHECK(Is(RustVocabFind("std::vec"), RustVocabKind::Module));
    CHECK(RustVocabFindAll("std::vec").size() == 2);
    // Clone: the trait and the derive.
    bool trait = false, derive = false;
    for (const RustVocabEntry *x : RustVocabFindAll("std::clone::Clone")) {
        trait |= x->kind == RustVocabKind::Trait;
        derive |= x->kind == RustVocabKind::Derive;
    }
    CHECK(trait && derive);
}

void TestMembers() {
    // Module members include re-exports (std::collections's BTreeMap comes from alloc).
    bool hashmap = false, btreemap = false, nested = false;
    for (const RustVocabModuleMember &m : RustVocabModuleMembers("std::collections")) {
        hashmap |= m.name == "HashMap" && m.entry->kind == RustVocabKind::Struct;
        btreemap |= m.name == "BTreeMap";
        nested |= m.name.find("::") != std::string_view::npos;
    }
    CHECK(hashmap && btreemap && !nested);
    bool read = false;
    for (const RustVocabModuleMember &m : RustVocabModuleMembers("std::io::prelude"))
        read |= m.name == "Read" && m.entry->kind == RustVocabKind::Trait;
    CHECK(read);
    CHECK(RustVocabModuleMembers("std::vec::Vec").empty());  // not a module

    // Own members: variants, fields, inherent methods, trait items.
    bool some = false;
    for (const RustVocabEntry *x : RustVocabOwnMembers("std::option::Option"))
        some |= x->kind == RustVocabKind::Variant && std::string_view(x->name) == "Some";
    CHECK(some);
    CHECK(Is(RustVocabFind("std::ops::Range::start"), RustVocabKind::Field));
    CHECK(Is(RustVocabFind("std::iter::Iterator::Item"), RustVocabKind::AssocType));
    CHECK(RustVocabOwnMembers("std::iter::Iterator").size() > 50);

    // Trait members through impls; blanket impls only on request.
    const std::vector<RustVocabMemberRef> vec = RustVocabTypeMembers("std::vec::Vec");
    CHECK(HasMember(vec, "push", ""));
    CHECK(HasMember(vec, "clone", "std::clone::Clone"));
    CHECK(HasMember(vec, "default", "std::default::Default"));
    CHECK(HasMember(vec, "into_iter", "std::iter::IntoIterator"));
    CHECK(HasMember(vec, "extend", "std::iter::Extend"));
    CHECK(!HasMember(vec, "to_string", "std::string::ToString"));
    CHECK(HasMember(RustVocabTypeMembers("std::string::String", true), "into", "std::convert::Into"));
    CHECK(HasMember(RustVocabTypeMembers("std::string::String", true), "to_string", "std::string::ToString"));

    // Deref: Vec -> [T] (slice methods), String -> str.
    CHECK(RustVocabDerefTarget("std::vec::Vec") == "slice");
    CHECK(RustVocabDerefTarget("alloc::string::String") == "str");
    CHECK(RustVocabDerefTarget("std::path::PathBuf") == "std::path::Path");
    CHECK(Is(RustVocabFindMember("slice", "iter"), RustVocabKind::Method));
    CHECK(Is(RustVocabFindMember("slice", "to_vec"), RustVocabKind::Method));  // alloc's impl [T]
    CHECK(Is(RustVocabFindMember("str", "to_uppercase"), RustVocabKind::Method));  // alloc's impl str
    CHECK(Is(RustVocabFindMember("f64", "sqrt"), RustVocabKind::Method));  // std's impl f64

    // Impl rows carry the associated types.
    bool into_iter = false;
    for (const RustVocabImpl *r : RustVocabImpls("std::vec::Vec"))
        if (std::string_view(r->trait) == "std::iter::IntoIterator" && Contains(r->assoc, "Item = T"))
            into_iter = true;
    CHECK(into_iter);
    CHECK(RustVocabImplementors("std::iter::Iterator").size() > 50);
    CHECK(!RustVocabImplements("std::rc::Rc", "std::marker::Send"));

    // Supertraits.
    bool partial = false;
    for (std::string_view s : RustVocabSupertraits("std::cmp::Ord")) partial |= s == "std::cmp::PartialOrd";
    CHECK(partial);
}

void TestPrelude() {
    auto has = [](int edition, std::string_view name) { return !RustVocabPreludeFind(name, edition).empty(); };
    CHECK(has(2015, "Vec") && has(2015, "Option") && has(2015, "Some") && has(2015, "Iterator"));
    CHECK(!has(2018, "TryFrom") && has(2021, "TryFrom") && has(2024, "TryFrom"));
    CHECK(!has(2021, "Future") && has(2024, "Future"));
    CHECK(has(2021, "FromIterator"));
    CHECK(!has(2021, "HashMap"));
    CHECK(RustVocabPreludeFind("Some", 2021)[0]->kind == RustVocabKind::Variant);
    CHECK(std::string_view(RustVocabPreludeFind("Some", 2021)[0]->path) == "std::option::Option::Some");
    CHECK(RustVocabPreludeFind("Clone", 2021).size() == 2);  // trait + derive
    CHECK(RustVocabPrelude(2024).size() > RustVocabPrelude(2015).size());
    CHECK(RustVocabFind("std::vec::Vec")->in_prelude);
    CHECK(!RustVocabFind("std::collections::HashMap")->in_prelude);
}

void TestFlags() {
    // Deprecated: std::mem::uninitialized; the old std::i32 module constants.
    const RustVocabEntry *e = RustVocabFind("std::mem::uninitialized");
    CHECK(e != nullptr && e->deprecated);
    CHECK(Contains(e->doc, "Deprecated since"));
    CHECK(!RustVocabFind("std::vec::Vec::push")->deprecated);
    // Unstable items are not listed.
    CHECK(RustVocabFindMember("std::vec::Vec", "push_within_capacity") == nullptr);
    CHECK(RustVocabFind("std::intrinsics") == nullptr);
    // An unstable const fn is shown without `const`.
    CHECK(!Contains(RustVocabFind("std::vec::Vec::push")->signature, "const"));
    CHECK(Contains(RustVocabFind("std::vec::Vec::new")->signature, "pub const fn new()"));
    // Never a trigraph or a raw HTML entity in the docs.
    for (size_t i = 0; i < RustVocabSize(); ++i) CHECK(!Contains(RustVocabAt(i).doc, "&minus;"));
    CHECK(std::string_view(RustVocabKindName(RustVocabKind::AssocFn)) == "associated function");
}

}  // namespace

int main() {
    TestTable();
    TestSpotChecks();
    TestPaths();
    TestMembers();
    TestPrelude();
    TestFlags();
    std::printf("rust_lsp_vocab_test: %d checks passed\n", g_checks);
    return 0;
}

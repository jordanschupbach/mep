#pragma once

// mep's own in-house PDF object-model parser -- see PDFIUM_REMOVAL_PLAN.md
// Phase 2. Parses PDF's object syntax (ISO 32000-1 7.3: null/boolean/
// numeric/string/name/array/dictionary/stream/indirect-reference objects)
// directly out of a byte buffer at a given offset. Deliberately
// xref-independent: this module knows nothing about object offsets,
// cross-reference tables, or the page tree (PDFIUM_REMOVAL_PLAN.md's
// Phase 3) -- it only turns "bytes at position N" into an Object or an
// IndirectObject. Resolving an indirect /Length (a stream dict whose
// length is itself `5 0 R` rather than a literal integer) needs the xref
// machinery Phase 3 builds, so ParseIndirectObject takes an optional
// LengthResolver callback instead of assuming one exists yet -- with no
// resolver (or one that fails), it falls back to scanning forward for the
// next "endstream" keyword, matching pdf_doc.h's documented tolerance for
// malformed/nonconforming content rather than failing outright.
//
// Not a general-purpose PDF value type in the sense of preserving object
// identity/sharing -- Array/Dict nest Objects by value (not indirectly
// through the object graph; indirect references stay as Reference
// objects, resolved later by whatever holds the document-wide object
// cache, Phase 3's PdfDocument). That keeps this module a pure,
// allocation-simple recursive-descent parser with no ownership subtleties.

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pdfobj {

enum class Type {
    Null,
    Bool,
    Int,
    Real,
    String,     // decoded bytes (escapes/hex already resolved) -- may contain arbitrary bytes, not necessarily text
    Name,       // decoded text, without the leading '/', #xx escapes already resolved
    Array,
    Dict,
    Reference,  // "N G R"
};

struct Object;

// An indirect reference's target: object number + generation.
struct Ref {
    int num = 0;
    int gen = 0;
};

// A PDF object value. Exactly one of the fields below is meaningful,
// selected by `type` -- see the accessors, which return a sane default
// (matching PdfDoc::RenderPage's own "tolerant of malformed content"
// contract elsewhere in this codebase) rather than asserting when called
// against the wrong type.
struct Object {
    Type type = Type::Null;

    bool bool_val = false;
    long long int_val = 0;
    double real_val = 0.0;
    std::string str_val;  // String and Name payload
    std::vector<Object> array_val;
    std::map<std::string, Object> dict_val;  // keyed by Name text (no leading '/')
    Ref ref_val;

    bool IsNull() const { return type == Type::Null; }
    bool IsNumber() const { return type == Type::Int || type == Type::Real; }
    bool IsDict() const { return type == Type::Dict; }
    bool IsArray() const { return type == Type::Array; }
    bool IsName() const { return type == Type::Name; }
    bool IsString() const { return type == Type::String; }
    bool IsReference() const { return type == Type::Reference; }

    // Numeric value as a double regardless of whether this parsed as an
    // Int or a Real token -- PDF itself doesn't distinguish "3" from
    // "3.0" semantically (e.g. either can appear in a /MediaBox), so
    // call sites almost never care which. Returns `def` for a non-number.
    double AsDouble(double def = 0.0) const;
    // Truncating integer accessor (e.g. /Width, /BitsPerComponent) --
    // returns `def` for a non-number.
    long long AsInt(long long def = 0) const;
    // Decoded name/string text, or `def` if this isn't a Name/String.
    // Returns by value (not a reference to `def`) since `def` is
    // routinely a temporary at the call site (e.g. `obj.AsString("")`)
    // -- a reference-returning overload would dangle the moment the
    // caller held onto it past this call.
    std::string AsString(const std::string &def) const;

    // Dict-only: looks up `key`, returning nullptr if this isn't a Dict
    // or the key is absent. Never resolves References -- callers that
    // need "follow this if it's a reference" go through Phase 3's object
    // resolver, which wraps this.
    const Object *Find(const std::string &key) const;
};

// Skips PDF whitespace (NUL/tab/LF/FF/CR/space) and `%`-to-end-of-line
// comments starting at data[pos], advancing pos past all of it.
void SkipWhitespaceAndComments(const unsigned char *data, size_t len, size_t &pos);

// Parses one object value starting at data[pos] (after skipping leading
// whitespace/comments itself), advancing pos past it. Returns false
// (leaving *out as a default-constructed Null Object and pos
// unadvanced) if pos is at/past len or the bytes there don't start a
// recognizable object -- callers decide whether that's fatal or
// skippable, matching this codebase's general "skip bad content, don't
// abort the whole document" approach (see RenderPage's own doc comment
// in pdf_doc.h).
bool ParseObject(const unsigned char *data, size_t len, size_t &pos, Object *out);

// Resolves an indirect object's /Length to a byte count. Returns false
// if it can't (e.g. the reference target isn't parseable yet, or no
// resolver was given) -- ParseIndirectObject then falls back to an
// endstream scan.
using LengthResolver = std::function<bool(int num, int gen, long long *out_length)>;

// One "N G obj ... endobj" body, including its raw (not yet
// filter-decoded -- that's Phase 4) stream bytes if it has one.
struct IndirectObject {
    int num = 0;
    int gen = 0;
    Object value;  // the dict/array/number/etc. between "obj" and "endobj" (or "stream")
    bool has_stream = false;
    size_t stream_offset = 0;  // byte offset into the original buffer where raw stream data starts
    size_t stream_length = 0;  // raw byte length (before any /Filter is applied)
};

// Parses the indirect object starting at data[pos] (which must point at
// the leading digit of "N G obj", not at whitespace before it -- callers
// typically get `pos` from an xref entry's byte offset, which already
// points there). Returns false if the header itself ("N G obj") isn't
// recognizable; a missing "endobj"/malformed stream length is tolerated
// (see LengthResolver's own doc comment) rather than failing the whole
// object.
bool ParseIndirectObject(const unsigned char *data, size_t len, size_t pos, IndirectObject *out,
                          const LengthResolver &resolve_length = nullptr);

}  // namespace pdfobj

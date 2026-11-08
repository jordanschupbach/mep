#ifndef MEP_JAVA_LSP_VOCAB_H
#define MEP_JAVA_LSP_VOCAB_H

// The JDK's public API for mep-java-lsp: every public/protected type of the
// exported packages of the java.* modules and the common jdk.* ones, with
// their fields, enum constants, constructors and methods (generic
// signatures, parameter names, deprecation, constant values). Names and
// signatures only -- no javadoc prose (that is GPLv2+CPE); hover links to
// docs.oracle.com instead. The table (java_lsp_vocab_data.cpp) is generated
// by tools/gen_java_vocab.java (`just java-lsp-vocab`) from the pinned JDK 25.
//
// Each row is one type: a qualified canonical name ("java.util.Map.Entry")
// and a text blob (see the generator's header for the encoding), parsed on
// first use. Type strings in the blob are qualified except java.lang's
// top-level types, which are bare ("String", "Thread.State").

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct JavaVocabRow {
    const char *name;  // qualified canonical name; a long type spans consecutive rows
    const char *data;
};

struct JavaVocabMember {
    char kind = 'm';  // 'f' field, 'k' enum constant, 'm' method, 'c' constructor
    std::string flags;  // s static, a abstract, f final, d default, p protected, v varargs, D deprecated, R for removal
    std::string name;
    std::string type_params;  // "T extends Comparable<? super T>;U" (';'-joined)
    std::string type;         // field type / return type; "" for a constructor
    std::string params;       // "int index;E element" (';'-joined "type name")
    std::string throws_;      // ';'-joined
    std::string value;        // constant value or annotation default, as Java source
    bool Has(char f) const { return flags.find(f) != std::string::npos; }
    /** @brief Each "type name" parameter split into (type, name). */
    std::vector<std::pair<std::string, std::string>> Params() const;
};

struct JavaVocabType {
    std::string qname;    // "java.util.Map.Entry"
    std::string module;   // "java.base"
    std::string package;  // "java.util"
    std::string simple;   // "Entry"
    std::string nested;   // "Map.Entry" (qname without the package)
    char kind = 'c';      // c class, i interface, e enum, r record, a annotation
    std::string flags;    // a abstract, f final, s static, S sealed, p protected, F functional, D, R
    std::string type_params;
    std::string super;       // superclass ("" for interfaces and Object)
    std::string interfaces;  // ';'-joined
    std::vector<JavaVocabMember> members;
    bool Has(char f) const { return flags.find(f) != std::string::npos; }
};

/** @brief The type with this qualified name ("java.util.List", "java.util.Map.Entry",
 *  or a bare java.lang name "String"), parsed once and cached; nullptr if unknown. */
const JavaVocabType *JavaVocabFind(std::string_view qname);
/** @brief Whether a package (exactly) has types in the table. */
bool JavaVocabIsPackage(std::string_view pkg);
/** @brief Every package with types, sorted. */
const std::vector<std::string> &JavaVocabPackages();
/** @brief The top-level types of a package, qualified names, sorted. */
std::vector<std::string> JavaVocabPackageTypes(std::string_view pkg);
/** @brief The member types directly nested in a type ("java.util.Map" -> "java.util.Map.Entry"). */
std::vector<std::string> JavaVocabNestedTypes(std::string_view qname);
/** @brief Qualified names of every type (top-level or nested) with this simple name. */
std::vector<std::string> JavaVocabBySimpleName(std::string_view simple);
/** @brief A vocabulary type string made readable: packages dropped, ',' -> ", ", ';' -> ", ". */
std::string JavaVocabDisplayType(std::string_view t);
/** @brief The qualified form of a blob type name ("String" -> "java.lang.String"). */
std::string JavaVocabQualify(std::string_view t);
/** @brief The docs.oracle.com (Java SE 25) page of a type, with an anchor for a member if given. */
std::string JavaVocabDocUrl(const JavaVocabType &t, const JavaVocabMember *m = nullptr);
/** @brief A member as a Java declaration ("public static <T> List<T> of(T... elements)"). */
std::string JavaVocabMemberSignature(const JavaVocabType &t, const JavaVocabMember &m);
/** @brief A type as a Java declaration header ("public interface List<E> extends SequencedCollection<E>"). */
std::string JavaVocabTypeSignature(const JavaVocabType &t);
/** @brief Number of types. */
size_t JavaVocabSize();

#endif

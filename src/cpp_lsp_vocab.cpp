// The C++ vocabulary mep's own language server carries: keywords,
// preprocessor directives, the standard headers, and the standard-library
// names it can say something useful about.
//
// Two tiers, deliberately:
//
//   1. The **generated** name list (src/cpp_lsp_std_names.cpp, 4,000-odd
//      entries, produced by scripts/gen_cpp_std_names.sh from the real
//      standard headers on the machine that ran it). It is used for
//      positive knowledge only -- "this name exists" -- which is what
//      keeps the undefined-name check from reporting every standard
//      facility nobody bothered to curate. A name missing from it is
//      never reported as an error; a name in it is simply never
//      questioned.
//
//   2. The **curated** tables below: a signature and a sentence for the
//      names a person actually types, and member lists for the handful
//      of types whose members a completion after `.` should offer. Every
//      signature here was checked against the headers it describes, the
//      way r_lsp_vocab.cpp's were checked with `formals(args(fn))` --
//      writing them from memory is how a table like this ends up
//      confidently wrong.
//
// What is NOT here: anything about a type's behaviour that would need a
// compiler to be sure of. This table says `std::vector` has `push_back`;
// it never says what `v.push_back(x)` does to the type of `v`.

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cpp_lsp.h"

// The generated list (see the header comment above).
extern const char *const kCppStdNames[];
extern const unsigned kCppStdNameCount;

namespace {

const std::vector<CppLspVocabEntry> &MakeKeywordVocab() {
    static const std::vector<CppLspVocabEntry> kVocab = {
        {"alignas", "alignas(N)", "Sets the alignment of the declaration that follows."},
        {"alignof", "alignof(T)", "Yields the alignment requirement of a type."},
        {"asm", "asm(\"...\")", "Embeds assembly; the form is the compiler's, not the standard's."},
        {"auto", "auto", "Deduces the declared type from the initializer."},
        {"bool", "bool", "Boolean type: `true` or `false`."},
        {"break", "break;", "Leaves the innermost loop or switch."},
        {"case", "case value:", "A branch of a switch."},
        {"catch", "catch (const E &e)", "Handles an exception thrown in the matching try block."},
        {"char", "char", "Narrow character type, one byte."},
        {"char8_t", "char8_t", "UTF-8 code unit type (C++20)."},
        {"char16_t", "char16_t", "UTF-16 code unit type."},
        {"char32_t", "char32_t", "UTF-32 code unit type."},
        {"class", "class Name { ... };", "Declares a class; members are private by default."},
        {"co_await", "co_await expr", "Suspends a coroutine until the awaited operation is ready."},
        {"co_return", "co_return expr;", "Finishes a coroutine, producing its result."},
        {"co_yield", "co_yield expr;", "Produces a value from a coroutine and suspends it."},
        {"concept", "template <class T> concept C = ...;", "Names a compile-time constraint on a type."},
        {"const", "const T", "The object may not be modified through this name."},
        {"consteval", "consteval T f();", "The function must be evaluated at compile time."},
        {"constexpr", "constexpr T x = ...;", "May be evaluated at compile time."},
        {"constinit", "constinit T x = ...;", "Requires constant initialization, without implying const."},
        {"const_cast", "const_cast<T>(e)", "Adds or removes const/volatile; changes nothing else."},
        {"continue", "continue;", "Starts the next iteration of the innermost loop."},
        {"decltype", "decltype(e)", "The declared type of an expression, without evaluating it."},
        {"default", "default:", "The fallback branch of a switch, or `= default` for a compiler-written member."},
        {"delete", "delete p;", "Destroys an object created with new, or `= delete` to remove a member."},
        {"do", "do { ... } while (cond);", "A loop whose body runs before the condition is tested."},
        {"double", "double", "Double-precision floating point."},
        {"dynamic_cast", "dynamic_cast<T>(e)", "Checked downcast through a polymorphic base."},
        {"else", "else", "The branch taken when an if's condition is false."},
        {"enum", "enum class Name { ... };", "An enumeration; `enum class` scopes its enumerators."},
        {"explicit", "explicit C(T);", "Stops a constructor or conversion from being used implicitly."},
        {"export", "export module m;", "Exports a module or one of its declarations (C++20)."},
        {"extern", "extern T x;", "Declares something defined elsewhere, or names a linkage."},
        {"false", "false", "The boolean false."},
        {"float", "float", "Single-precision floating point."},
        {"for", "for (init; cond; step)", "A counted loop, or `for (x : range)` over a range."},
        {"friend", "friend class C;", "Grants another class or function access to private members."},
        {"goto", "goto label;", "Jumps to a label in the same function."},
        {"if", "if (cond)", "Runs a branch when a condition holds; `if constexpr` chooses at compile time."},
        {"inline", "inline T f()", "Permits several identical definitions across translation units."},
        {"int", "int", "The natural signed integer type."},
        {"long", "long", "A wider signed integer type."},
        {"mutable", "mutable T x;", "A member that may be modified through a const object."},
        {"namespace", "namespace name { ... }", "Groups declarations under a name."},
        {"new", "new T(args)", "Creates an object with dynamic storage duration."},
        {"noexcept", "noexcept", "Declares that a function throws nothing, or asks whether an expression can."},
        {"nullptr", "nullptr", "The null pointer constant."},
        {"operator", "operator+", "Names an overloaded operator or a conversion function."},
        {"private", "private:", "Members below are accessible only inside the class."},
        {"protected", "protected:", "Members below are accessible in the class and its derived classes."},
        {"public", "public:", "Members below are accessible everywhere."},
        {"register", "register T x;", "A storage class removed in C++17; no effect."},
        {"reinterpret_cast", "reinterpret_cast<T>(e)", "Reinterprets the bits of a pointer or integer."},
        {"requires", "requires (T t) { ... }", "States a constraint, or introduces a requires-expression."},
        {"return", "return expr;", "Leaves a function, with a value when it has one."},
        {"short", "short", "A narrower signed integer type."},
        {"signed", "signed", "The signed variant of an integer type."},
        {"sizeof", "sizeof(T)", "The size in bytes of a type or expression."},
        {"static", "static T x;", "Internal linkage at file scope, one shared copy in a class or function."},
        {"static_assert", "static_assert(cond, \"message\");", "Fails the build when a constant condition is false."},
        {"static_cast", "static_cast<T>(e)", "A conversion the language already knows how to do."},
        {"struct", "struct Name { ... };", "Declares a class whose members are public by default."},
        {"switch", "switch (value)", "Branches on an integral or enumeration value."},
        {"template", "template <class T>", "Declares a family of classes or functions parameterized by types."},
        {"this", "this", "A pointer to the object a member function was called on."},
        {"thread_local", "thread_local T x;", "One object per thread."},
        {"throw", "throw e;", "Raises an exception."},
        {"true", "true", "The boolean true."},
        {"try", "try { ... } catch (...) { ... }", "Runs a block with exception handlers attached."},
        {"typedef", "typedef T Name;", "Names a type; `using Name = T;` is the modern spelling."},
        {"typeid", "typeid(e)", "Yields the runtime type information of an expression."},
        {"typename", "typename T::type", "Says that a dependent name is a type."},
        {"union", "union Name { ... };", "A type whose members share storage."},
        {"unsigned", "unsigned", "The unsigned variant of an integer type."},
        {"using", "using Name = T;", "Introduces an alias, a declaration, or a whole namespace."},
        {"virtual", "virtual T f();", "Dispatches on the dynamic type; `= 0` makes it pure."},
        {"void", "void", "No type: a function returning nothing, or an untyped pointer."},
        {"volatile", "volatile T x;", "Every access must really happen; not a threading tool."},
        {"wchar_t", "wchar_t", "Wide character type."},
        {"while", "while (cond)", "A loop that tests its condition before each iteration."},
        // Identifiers with special meaning, offered because they are what
        // a person is reaching for even though the grammar calls them
        // ordinary names.
        {"override", "override", "States that this member overrides a virtual one; the compiler checks it."},
        {"final", "final", "Stops further overriding, or further derivation."},
        {"import", "import m;", "Imports a module (C++20)."},
        {"module", "module m;", "Declares this file part of a module (C++20)."},
    };
    return kVocab;
}

const std::vector<CppLspVocabEntry> &MakeDirectiveVocab() {
    static const std::vector<CppLspVocabEntry> kVocab = {
        {"include", "#include <header>", "Inserts another file here."},
        {"define", "#define NAME value", "Defines a macro; function-like with a parameter list."},
        {"undef", "#undef NAME", "Removes a macro definition."},
        {"if", "#if expression", "Compiles the group only when a constant expression is non-zero."},
        {"ifdef", "#ifdef NAME", "Compiles the group only when a macro is defined."},
        {"ifndef", "#ifndef NAME", "Compiles the group only when a macro is not defined."},
        {"elif", "#elif expression", "The next condition of an #if chain."},
        {"elifdef", "#elifdef NAME", "The next condition, testing for a definition (C++23)."},
        {"elifndef", "#elifndef NAME", "The next condition, testing for no definition (C++23)."},
        {"else", "#else", "The group compiled when every earlier condition failed."},
        {"endif", "#endif", "Closes the innermost #if."},
        {"pragma", "#pragma once", "A compiler-specific instruction; `once` is the portable include guard."},
        {"error", "#error \"message\"", "Fails the build with a message."},
        {"warning", "#warning \"message\"", "Emits a warning."},
        {"line", "#line 42 \"file\"", "Rewrites the line and file a diagnostic reports."},
    };
    return kVocab;
}

const std::vector<CppLspVocabEntry> &MakeHeaderVocab() {
    static const std::vector<CppLspVocabEntry> kVocab = {
        {"algorithm", "<algorithm>", "sort, find, copy, transform and the rest of the range algorithms."},
        {"any", "<any>", "std::any: a value of any copyable type."},
        {"array", "<array>", "std::array: a fixed-size array with a container interface."},
        {"atomic", "<atomic>", "std::atomic and the memory-order enumerations."},
        {"bit", "<bit>", "std::bit_cast, popcount, countl_zero (C++20)."},
        {"bitset", "<bitset>", "std::bitset: a fixed-size sequence of bits."},
        {"charconv", "<charconv>", "std::to_chars and from_chars: locale-free number conversion."},
        {"chrono", "<chrono>", "Clocks, durations and calendar types."},
        {"cmath", "<cmath>", "sqrt, sin, floor and the rest of the C math library."},
        {"compare", "<compare>", "The comparison categories `operator<=>` returns."},
        {"complex", "<complex>", "std::complex."},
        {"concepts", "<concepts>", "The standard concepts: same_as, integral, invocable."},
        {"condition_variable", "<condition_variable>", "std::condition_variable."},
        {"coroutine", "<coroutine>", "std::coroutine_handle and the promise interface."},
        {"cstdint", "<cstdint>", "int32_t, uint64_t, intptr_t and friends."},
        {"cstdio", "<cstdio>", "printf, fopen, FILE."},
        {"cstdlib", "<cstdlib>", "malloc, atoi, exit, abs."},
        {"cstring", "<cstring>", "memcpy, strlen, strcmp."},
        {"ctime", "<ctime>", "time_t, strftime, clock."},
        {"deque", "<deque>", "std::deque: a double-ended queue."},
        {"exception", "<exception>", "std::exception, terminate, current_exception."},
        {"expected", "<expected>", "std::expected: a value or an error (C++23)."},
        {"filesystem", "<filesystem>", "std::filesystem::path and the directory operations."},
        {"format", "<format>", "std::format: type-safe formatting (C++20)."},
        {"fstream", "<fstream>", "ifstream, ofstream, fstream."},
        {"functional", "<functional>", "std::function, bind, less, hash, invoke."},
        {"initializer_list", "<initializer_list>", "std::initializer_list."},
        {"iomanip", "<iomanip>", "setw, setprecision and the other stream manipulators."},
        {"ios", "<ios>", "The stream base class and its flags."},
        {"iostream", "<iostream>", "cin, cout, cerr."},
        {"istream", "<istream>", "std::istream."},
        {"iterator", "<iterator>", "begin, end, back_inserter, iterator_traits."},
        {"limits", "<limits>", "std::numeric_limits."},
        {"list", "<list>", "std::list: a doubly linked list."},
        {"map", "<map>", "std::map and std::multimap: ordered associative containers."},
        {"memory", "<memory>", "unique_ptr, shared_ptr, make_unique, allocator."},
        {"mutex", "<mutex>", "std::mutex, lock_guard, unique_lock, scoped_lock."},
        {"numbers", "<numbers>", "std::numbers::pi and the other mathematical constants (C++20)."},
        {"numeric", "<numeric>", "accumulate, iota, reduce, gcd."},
        {"optional", "<optional>", "std::optional: a value that may be absent."},
        {"ostream", "<ostream>", "std::ostream and `operator<<`."},
        {"queue", "<queue>", "std::queue and std::priority_queue."},
        {"random", "<random>", "mt19937 and the distributions."},
        {"ranges", "<ranges>", "The range views and range algorithms (C++20)."},
        {"ratio", "<ratio>", "std::ratio: a compile-time rational number."},
        {"regex", "<regex>", "std::regex and the matching algorithms."},
        {"set", "<set>", "std::set and std::multiset."},
        {"shared_mutex", "<shared_mutex>", "std::shared_mutex and shared_lock."},
        {"source_location", "<source_location>", "std::source_location (C++20)."},
        {"span", "<span>", "std::span: a non-owning view over contiguous storage (C++20)."},
        {"sstream", "<sstream>", "istringstream, ostringstream, stringstream."},
        {"stack", "<stack>", "std::stack."},
        {"stdexcept", "<stdexcept>", "runtime_error, logic_error, out_of_range."},
        {"string", "<string>", "std::string and std::to_string."},
        {"string_view", "<string_view>", "std::string_view: a non-owning view of characters."},
        {"system_error", "<system_error>", "std::error_code and std::system_error."},
        {"thread", "<thread>", "std::thread and this_thread."},
        {"tuple", "<tuple>", "std::tuple, make_tuple, tie, apply."},
        {"type_traits", "<type_traits>", "is_same, enable_if, decay and the rest of the traits."},
        {"typeindex", "<typeindex>", "std::type_index."},
        {"typeinfo", "<typeinfo>", "std::type_info, what `typeid` yields."},
        {"unordered_map", "<unordered_map>", "std::unordered_map: a hash table."},
        {"unordered_set", "<unordered_set>", "std::unordered_set."},
        {"utility", "<utility>", "move, forward, swap, pair, exchange."},
        {"variant", "<variant>", "std::variant: a type-safe union."},
        {"vector", "<vector>", "std::vector: the default sequence container."},
        {"version", "<version>", "Every standard library feature-test macro."},
        {"cassert", "<cassert>", "The assert macro."},
        {"cctype", "<cctype>", "isalpha, isdigit, tolower."},
        {"cerrno", "<cerrno>", "errno and its values."},
        {"cfloat", "<cfloat>", "DBL_MAX and the other floating-point limits."},
        {"climits", "<climits>", "INT_MAX and the other integer limits."},
        {"cstddef", "<cstddef>", "size_t, ptrdiff_t, nullptr_t, byte."},
    };
    return kVocab;
}

// The names worth a signature and a sentence. Everything else the
// standard library declares is still *known* (the generated list), it
// just has nothing extra to say.
const std::vector<CppLspVocabEntry> &MakeStdVocab() {
    static const std::vector<CppLspVocabEntry> kVocab = {
        {"vector", "template <class T, class Alloc = allocator<T>> class vector",
         "A contiguous, growable sequence. <vector>"},
        {"string", "class string", "A sequence of char with value semantics. <string>"},
        {"string_view", "class string_view", "A non-owning view of characters; does not own what it points at. "
                                             "<string_view>"},
        {"map", "template <class Key, class T, class Compare = less<Key>> class map",
         "An ordered associative container with unique keys. <map>"},
        {"unordered_map", "template <class Key, class T, class Hash = hash<Key>> class unordered_map",
         "A hash table with unique keys. <unordered_map>"},
        {"set", "template <class Key, class Compare = less<Key>> class set",
         "An ordered set of unique keys. <set>"},
        {"unordered_set", "template <class Key, class Hash = hash<Key>> class unordered_set",
         "A hash set of unique keys. <unordered_set>"},
        {"array", "template <class T, size_t N> class array", "A fixed-size array with a container interface. "
                                                              "<array>"},
        {"deque", "template <class T> class deque", "A double-ended queue. <deque>"},
        {"list", "template <class T> class list", "A doubly linked list. <list>"},
        {"pair", "template <class T1, class T2> struct pair", "Two values, `first` and `second`. <utility>"},
        {"tuple", "template <class... Types> class tuple", "A fixed-size collection of heterogeneous values. "
                                                           "<tuple>"},
        {"optional", "template <class T> class optional", "A value that may be absent; test it before reading it. "
                                                          "<optional>"},
        {"variant", "template <class... Types> class variant", "A type-safe union. <variant>"},
        {"unique_ptr", "template <class T, class D = default_delete<T>> class unique_ptr",
         "Sole ownership of a heap object, released when it goes out of scope. <memory>"},
        {"shared_ptr", "template <class T> class shared_ptr",
         "Shared ownership with a reference count. <memory>"},
        {"weak_ptr", "template <class T> class weak_ptr", "A non-owning observer of a shared_ptr. <memory>"},
        {"make_unique", "template <class T, class... Args> unique_ptr<T> make_unique(Args &&...args)",
         "Creates an object and wraps it in a unique_ptr. <memory>"},
        {"make_shared", "template <class T, class... Args> shared_ptr<T> make_shared(Args &&...args)",
         "Creates an object and its control block in one allocation. <memory>"},
        {"function", "template <class R, class... Args> class function<R(Args...)>",
         "A type-erased callable. <functional>"},
        {"move", "template <class T> constexpr remove_reference_t<T> &&move(T &&t)",
         "Casts to an rvalue reference, so the value may be moved from. <utility>"},
        {"forward", "template <class T> constexpr T &&forward(remove_reference_t<T> &t)",
         "Passes an argument on with its value category intact. <utility>"},
        {"swap", "template <class T> void swap(T &a, T &b)", "Exchanges two values. <utility>"},
        {"exchange", "template <class T, class U = T> T exchange(T &obj, U &&new_value)",
         "Replaces a value and returns the old one. <utility>"},
        {"sort", "template <class It, class Compare> void sort(It first, It last, Compare comp)",
         "Sorts a range in place; the comparator is a strict weak ordering. <algorithm>"},
        {"stable_sort", "template <class It, class Compare> void stable_sort(It first, It last, Compare comp)",
         "Sorts a range, keeping equal elements in their original order. <algorithm>"},
        {"find", "template <class It, class T> It find(It first, It last, const T &value)",
         "Returns an iterator to the first match, or `last`. <algorithm>"},
        {"find_if", "template <class It, class Pred> It find_if(It first, It last, Pred p)",
         "Returns an iterator to the first element satisfying a predicate. <algorithm>"},
        {"count", "template <class It, class T> ptrdiff_t count(It first, It last, const T &value)",
         "Counts the elements equal to a value. <algorithm>"},
        {"copy", "template <class It, class Out> Out copy(It first, It last, Out result)",
         "Copies a range; `result` must have room. <algorithm>"},
        {"transform", "template <class It, class Out, class Op> Out transform(It first, It last, Out out, Op op)",
         "Applies a function to a range, writing the results elsewhere. <algorithm>"},
        {"for_each", "template <class It, class F> F for_each(It first, It last, F f)",
         "Calls a function on every element. <algorithm>"},
        {"accumulate", "template <class It, class T> T accumulate(It first, It last, T init)",
         "Folds a range, starting from `init`. <numeric>"},
        {"min", "template <class T> const T &min(const T &a, const T &b)", "The smaller of two values. <algorithm>"},
        {"max", "template <class T> const T &max(const T &a, const T &b)", "The larger of two values. <algorithm>"},
        {"clamp", "template <class T> const T &clamp(const T &v, const T &lo, const T &hi)",
         "Confines a value to a range. <algorithm>"},
        {"min_element", "template <class It> It min_element(It first, It last)",
         "An iterator to the smallest element. <algorithm>"},
        {"max_element", "template <class It> It max_element(It first, It last)",
         "An iterator to the largest element. <algorithm>"},
        {"remove_if", "template <class It, class Pred> It remove_if(It first, It last, Pred p)",
         "Moves the elements to keep to the front and returns the new end -- erase the tail yourself. <algorithm>"},
        {"reverse", "template <class It> void reverse(It first, It last)", "Reverses a range in place. <algorithm>"},
        {"fill", "template <class It, class T> void fill(It first, It last, const T &value)",
         "Assigns a value to every element. <algorithm>"},
        {"all_of", "template <class It, class Pred> bool all_of(It first, It last, Pred p)",
         "Whether every element satisfies a predicate. <algorithm>"},
        {"any_of", "template <class It, class Pred> bool any_of(It first, It last, Pred p)",
         "Whether any element satisfies a predicate. <algorithm>"},
        {"none_of", "template <class It, class Pred> bool none_of(It first, It last, Pred p)",
         "Whether no element satisfies a predicate. <algorithm>"},
        {"lower_bound", "template <class It, class T> It lower_bound(It first, It last, const T &value)",
         "The first position a value could be inserted at, in a sorted range. <algorithm>"},
        {"to_string", "string to_string(int value)", "Formats a number as a string. <string>"},
        {"stoi", "int stoi(const string &str, size_t *pos = nullptr, int base = 10)",
         "Parses an int; throws invalid_argument or out_of_range. <string>"},
        {"stod", "double stod(const string &str, size_t *pos = nullptr)", "Parses a double. <string>"},
        {"cout", "ostream cout", "The standard output stream. <iostream>"},
        {"cerr", "ostream cerr", "The standard error stream, unbuffered. <iostream>"},
        {"cin", "istream cin", "The standard input stream. <iostream>"},
        {"endl", "ostream &endl(ostream &os)", "Writes a newline and flushes -- `'\\n'` when you do not need the "
                                               "flush. <ostream>"},
        {"size_t", "using size_t = unsigned long", "The unsigned type a `sizeof` yields. <cstddef>"},
        {"ptrdiff_t", "using ptrdiff_t = long", "The signed type a pointer difference yields. <cstddef>"},
        {"nullptr_t", "using nullptr_t = decltype(nullptr)", "The type of `nullptr`. <cstddef>"},
        {"int8_t", "using int8_t = signed char", "An exactly 8-bit signed integer. <cstdint>"},
        {"int16_t", "using int16_t = short", "An exactly 16-bit signed integer. <cstdint>"},
        {"int32_t", "using int32_t = int", "An exactly 32-bit signed integer. <cstdint>"},
        {"int64_t", "using int64_t = long", "An exactly 64-bit signed integer. <cstdint>"},
        {"uint8_t", "using uint8_t = unsigned char", "An exactly 8-bit unsigned integer. <cstdint>"},
        {"uint16_t", "using uint16_t = unsigned short", "An exactly 16-bit unsigned integer. <cstdint>"},
        {"uint32_t", "using uint32_t = unsigned int", "An exactly 32-bit unsigned integer. <cstdint>"},
        {"uint64_t", "using uint64_t = unsigned long", "An exactly 64-bit unsigned integer. <cstdint>"},
        {"runtime_error", "class runtime_error : public exception",
         "An error detectable only at run time. <stdexcept>"},
        {"logic_error", "class logic_error : public exception",
         "An error in the program's own logic. <stdexcept>"},
        {"out_of_range", "class out_of_range : public logic_error", "An index or key outside its range. <stdexcept>"},
        {"invalid_argument", "class invalid_argument : public logic_error",
         "An argument a function cannot accept. <stdexcept>"},
        {"exception", "class exception", "The base of the standard exception hierarchy; `what()` describes it. "
                                         "<exception>"},
        {"thread", "class thread", "A thread of execution; join or detach it before it is destroyed. <thread>"},
        {"mutex", "class mutex", "A non-recursive mutual-exclusion lock. <mutex>"},
        {"lock_guard", "template <class M> class lock_guard", "Locks a mutex for a scope. <mutex>"},
        {"unique_lock", "template <class M> class unique_lock",
         "A movable scoped lock that may be unlocked early. <mutex>"},
        {"atomic", "template <class T> struct atomic", "An object whose operations are indivisible. <atomic>"},
        {"numeric_limits", "template <class T> class numeric_limits",
         "The properties of an arithmetic type: max(), min(), epsilon(). <limits>"},
        {"printf", "int printf(const char *format, ...)", "Formatted output to stdout. <cstdio>"},
        {"fprintf", "int fprintf(FILE *stream, const char *format, ...)", "Formatted output to a stream. <cstdio>"},
        {"snprintf", "int snprintf(char *s, size_t n, const char *format, ...)",
         "Formatted output into a buffer, with a size limit. <cstdio>"},
        {"memcpy", "void *memcpy(void *dest, const void *src, size_t n)",
         "Copies bytes; the regions may not overlap. <cstring>"},
        {"memset", "void *memset(void *s, int c, size_t n)", "Fills bytes with a value. <cstring>"},
        {"strlen", "size_t strlen(const char *s)", "The length of a null-terminated string. <cstring>"},
        {"malloc", "void *malloc(size_t size)", "Allocates raw memory. <cstdlib>"},
        {"free", "void free(void *ptr)", "Releases memory from malloc. <cstdlib>"},
        {"abs", "int abs(int n)", "Absolute value. <cstdlib>"},
        {"sqrt", "double sqrt(double x)", "Square root. <cmath>"},
        {"pow", "double pow(double base, double exp)", "Raises a number to a power. <cmath>"},
        {"floor", "double floor(double x)", "Rounds towards negative infinity. <cmath>"},
        {"ceil", "double ceil(double x)", "Rounds towards positive infinity. <cmath>"},
        {"round", "double round(double x)", "Rounds to the nearest integer, halves away from zero. <cmath>"},
        {"assert", "assert(condition)", "Aborts when the condition is false -- and does nothing at all under "
                                        "NDEBUG. <cassert>"},
    };
    return kVocab;
}

// Members offered after a `.` or `->` on a value of a known standard
// type. Not exhaustive by design: the ones a person reaches for, with the
// signature they actually need at the call site.
const std::map<std::string, std::vector<CppLspVocabEntry>> &MakeTypeMembers() {
    static const std::map<std::string, std::vector<CppLspVocabEntry>> kMembers = {
        {"string",
         {
             {"size", "size_t size() const", "The number of characters."},
             {"length", "size_t length() const", "The number of characters; the same as size()."},
             {"empty", "bool empty() const", "Whether the string has no characters."},
             {"c_str", "const char *c_str() const", "A null-terminated view of the contents."},
             {"data", "char *data()", "A pointer to the characters, null-terminated since C++11."},
             {"substr", "string substr(size_t pos = 0, size_t count = npos) const", "A copy of part of the string."},
             {"find", "size_t find(const string &str, size_t pos = 0) const",
              "The position of the first match, or `npos`."},
             {"rfind", "size_t rfind(const string &str, size_t pos = npos) const",
              "The position of the last match, or `npos`."},
             {"replace", "string &replace(size_t pos, size_t count, const string &str)",
              "Replaces part of the string."},
             {"append", "string &append(const string &str)", "Adds to the end."},
             {"push_back", "void push_back(char c)", "Adds one character to the end."},
             {"pop_back", "void pop_back()", "Removes the last character."},
             {"insert", "string &insert(size_t pos, const string &str)", "Inserts at a position."},
             {"erase", "string &erase(size_t pos = 0, size_t count = npos)", "Removes part of the string."},
             {"clear", "void clear()", "Removes every character."},
             {"resize", "void resize(size_t count, char c = char())", "Changes the length."},
             {"reserve", "void reserve(size_t new_cap)", "Reserves capacity without changing the length."},
             {"capacity", "size_t capacity() const", "How much room is allocated."},
             {"begin", "iterator begin()", "An iterator to the first character."},
             {"end", "iterator end()", "An iterator past the last character."},
             {"front", "char &front()", "The first character."},
             {"back", "char &back()", "The last character."},
             {"at", "char &at(size_t pos)", "The character at a position, bounds-checked."},
             {"compare", "int compare(const string &str) const", "Three-way lexicographic comparison."},
             {"starts_with", "bool starts_with(string_view sv) const", "Whether the string begins with a prefix "
                                                                       "(C++20)."},
             {"ends_with", "bool ends_with(string_view sv) const", "Whether the string ends with a suffix (C++20)."},
             {"npos", "static const size_t npos", "The \"not found\" position find() returns."},
         }},
        {"vector",
         {
             {"size", "size_t size() const", "The number of elements."},
             {"empty", "bool empty() const", "Whether the vector has no elements."},
             {"push_back", "void push_back(const T &value)", "Adds an element to the end."},
             {"emplace_back", "template <class... Args> T &emplace_back(Args &&...args)",
              "Constructs an element in place at the end."},
             {"pop_back", "void pop_back()", "Removes the last element."},
             {"insert", "iterator insert(const_iterator pos, const T &value)", "Inserts before a position."},
             {"emplace", "template <class... Args> iterator emplace(const_iterator pos, Args &&...args)",
              "Constructs an element in place before a position."},
             {"erase", "iterator erase(const_iterator pos)", "Removes an element, returning the next one."},
             {"clear", "void clear()", "Removes every element."},
             {"resize", "void resize(size_t count)", "Changes the number of elements."},
             {"reserve", "void reserve(size_t new_cap)", "Reserves capacity without changing the size."},
             {"capacity", "size_t capacity() const", "How many elements fit before reallocating."},
             {"shrink_to_fit", "void shrink_to_fit()", "Asks for the capacity to match the size."},
             {"data", "T *data()", "A pointer to the contiguous storage."},
             {"front", "T &front()", "The first element."},
             {"back", "T &back()", "The last element."},
             {"at", "T &at(size_t pos)", "The element at a position, bounds-checked."},
             {"begin", "iterator begin()", "An iterator to the first element."},
             {"end", "iterator end()", "An iterator past the last element."},
             {"rbegin", "reverse_iterator rbegin()", "A reverse iterator to the last element."},
             {"rend", "reverse_iterator rend()", "A reverse iterator before the first element."},
             {"assign", "void assign(size_t count, const T &value)", "Replaces the contents."},
             {"swap", "void swap(vector &other)", "Exchanges contents with another vector."},
         }},
        {"map",
         {
             {"size", "size_t size() const", "The number of elements."},
             {"empty", "bool empty() const", "Whether the map has no elements."},
             {"find", "iterator find(const Key &key)", "An iterator to the element, or end()."},
             {"count", "size_t count(const Key &key) const", "1 when the key is present, 0 otherwise."},
             {"contains", "bool contains(const Key &key) const", "Whether the key is present (C++20)."},
             {"at", "T &at(const Key &key)", "The mapped value, throwing out_of_range when absent."},
             {"insert", "pair<iterator, bool> insert(const value_type &value)",
              "Inserts unless the key is already there; the bool says which happened."},
             {"emplace", "template <class... Args> pair<iterator, bool> emplace(Args &&...args)",
              "Constructs an element in place unless the key is already there."},
             {"try_emplace", "template <class... Args> pair<iterator, bool> try_emplace(const Key &k, Args &&...args)",
              "Like emplace, but does not build the value when the key is present."},
             {"erase", "size_t erase(const Key &key)", "Removes a key, returning how many were removed."},
             {"clear", "void clear()", "Removes every element."},
             {"begin", "iterator begin()", "An iterator to the first element, in key order."},
             {"end", "iterator end()", "An iterator past the last element."},
             {"lower_bound", "iterator lower_bound(const Key &key)", "The first element not less than a key."},
             {"upper_bound", "iterator upper_bound(const Key &key)", "The first element greater than a key."},
         }},
        {"unique_ptr",
         {
             {"get", "T *get() const", "The raw pointer, without giving up ownership."},
             {"reset", "void reset(T *p = nullptr)", "Destroys what is held and takes the new pointer."},
             {"release", "T *release()", "Gives up ownership and returns the pointer."},
             {"swap", "void swap(unique_ptr &other)", "Exchanges ownership."},
         }},
        {"shared_ptr",
         {
             {"get", "T *get() const", "The raw pointer, without changing the count."},
             {"reset", "void reset()", "Drops this reference."},
             {"use_count", "long use_count() const", "How many shared_ptrs share the object -- a debugging aid, "
                                                     "not a synchronization one."},
             {"unique", "bool unique() const", "Whether the count is 1 (removed in C++20)."},
         }},
        {"optional",
         {
             {"has_value", "bool has_value() const", "Whether a value is present."},
             {"value", "T &value()", "The value, throwing bad_optional_access when absent."},
             {"value_or", "T value_or(U &&default_value) const", "The value, or a fallback."},
             {"reset", "void reset()", "Makes the optional empty."},
             {"emplace", "template <class... Args> T &emplace(Args &&...args)", "Constructs a value in place."},
         }},
        {"pair",
         {
             {"first", "T1 first", "The first member."},
             {"second", "T2 second", "The second member."},
             {"swap", "void swap(pair &other)", "Exchanges both members."},
         }},
    };
    return kMembers;
}

/** @brief Strips a `std::` qualifier and any template arguments from a type's spelling. */
std::string BareTypeName(const std::string &type) {
    std::string name = type;
    const size_t angle = name.find('<');
    if (angle != std::string::npos) name = name.substr(0, angle);
    const size_t colons = name.rfind("::");
    if (colons != std::string::npos) name = name.substr(colons + 2);
    return name;
}

}  // namespace

const std::vector<CppLspVocabEntry> &CppLspKeywordVocab() { return MakeKeywordVocab(); }
const std::vector<CppLspVocabEntry> &CppLspDirectiveVocab() { return MakeDirectiveVocab(); }
const std::vector<CppLspVocabEntry> &CppLspHeaderVocab() { return MakeHeaderVocab(); }
const std::vector<CppLspVocabEntry> &CppLspStdVocab() { return MakeStdVocab(); }

const std::vector<CppLspVocabEntry> *CppLspTypeMembers(const std::string &type) {
    const std::string bare = BareTypeName(type);
    const auto &members = MakeTypeMembers();
    const auto found = members.find(bare);
    return found == members.end() ? nullptr : &found->second;
}

bool CppLspIsKnownName(const std::string &name) {
    if (name.empty()) return false;
    // The generated list is sorted in byte order, so this is a binary
    // search over `const char *` with strcmp -- the same arrangement
    // r_lsp_base_names.cpp uses, and for the same reason.
    unsigned low = 0;
    unsigned high = kCppStdNameCount;
    while (low < high) {
        const unsigned mid = low + (high - low) / 2;
        const int order = std::strcmp(kCppStdNames[mid], name.c_str());
        if (order == 0) return true;
        if (order < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    for (const CppLspVocabEntry &entry : MakeStdVocab()) {
        if (name == entry.name) return true;
    }
    for (const CppLspVocabEntry &entry : MakeKeywordVocab()) {
        if (name == entry.name) return true;
    }
    return false;
}

std::string CppLspHeaderForName(const std::string &name) {
    // The curated entries end their documentation with the header they
    // come from, in angle brackets: one place to keep it right rather
    // than two tables that can disagree.
    for (const CppLspVocabEntry &entry : MakeStdVocab()) {
        if (name != entry.name) continue;
        const std::string doc = entry.doc;
        const size_t open = doc.rfind('<');
        const size_t close = doc.rfind('>');
        if (open == std::string::npos || close == std::string::npos || close < open) return "";
        return doc.substr(open + 1, close - open - 1);
    }
    return "";
}

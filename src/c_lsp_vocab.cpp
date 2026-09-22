// The hand-written half of mep's C language server's vocabulary: the
// keywords, the preprocessor directives, and a sentence of explanation
// for the standard-library names worth explaining.
//
// The *signatures* are not here. They come from src/c_lsp_std_names.cpp,
// which scripts/gen_c_names.py reads out of the real headers, because a
// remembered signature is a wrong signature (that script's header
// comment says so at length). What is here is the part no header
// contains: what the thing is for, in one line, for a reader who knows
// C but not this corner of it. A name absent from the doc table still
// gets its real declaration as `detail`; it just gets no prose.

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "c_lsp.h"
#include "c_lsp_std_names.h"

namespace {

// Every C keyword, with what it does. `_Bool`-style spellings included,
// and so are the C23 words that are still macros in most code (`bool`,
// `true`, `false`) -- offering those as keywords costs nothing and
// helps, which is a different question from whether the *parser* treats
// them as reserved (c_ast.cpp explains why it must not).
const std::vector<CLspVocabEntry> &Keywords() {
    static const std::vector<CLspVocabEntry> kVocab = {
        {"auto", "storage class", "The default storage class for a block-scope object. Writing it changes nothing."},
        {"break", "jump", "Leaves the innermost enclosing loop or `switch`."},
        {"case", "label", "A `switch` label. Its value must be an integer constant expression."},
        {"char", "type", "The smallest addressable integer type, one byte. Whether it is signed is up to the "
                         "implementation, so use `signed char` or `unsigned char` when it matters."},
        {"const", "qualifier", "Promises this object is not modified through this name. It does not make the "
                               "storage read-only."},
        {"continue", "jump", "Skips to the next iteration of the innermost enclosing loop."},
        {"default", "label", "The `switch` label taken when no `case` matches."},
        {"do", "statement", "`do { ... } while (cond);` -- a loop whose body always runs at least once."},
        {"double", "type", "A double-precision floating-point number."},
        {"else", "statement", "The branch an `if` takes when its condition is false."},
        {"enum", "type", "A set of named integer constants. The constants live in the ordinary name space, not "
                         "inside the enum."},
        {"extern", "storage class", "Declares a name that is defined somewhere else. A declaration, not a "
                                    "definition -- no storage is set aside."},
        {"float", "type", "A single-precision floating-point number."},
        {"for", "statement", "`for (init; cond; step)` -- a loop with its three parts written together."},
        {"goto", "jump", "Jumps to a label in the same function."},
        {"if", "statement", "Runs its body when the condition is non-zero."},
        {"inline", "function specifier", "A hint that calls should be expanded. It also changes the linkage "
                                         "rules, which is the part that actually bites."},
        {"int", "type", "The natural integer type for the machine, at least 16 bits."},
        {"long", "type", "A longer integer type, at least 32 bits. `long long` is at least 64."},
        {"register", "storage class", "Asks for the object to live in a register, and forbids taking its "
                                      "address. Compilers have ignored the first half for decades."},
        {"restrict", "qualifier", "Promises that, for the lifetime of this pointer, the object it points at is "
                                  "reached only through it. Breaking the promise is undefined behaviour."},
        {"return", "jump", "Leaves the function, with a value unless the function returns `void`."},
        {"short", "type", "A shorter integer type, at least 16 bits."},
        {"signed", "type", "Makes an integer type explicitly signed; only meaningful on `char`."},
        {"sizeof", "operator", "The size in bytes of a type or of an expression's type. The expression is not "
                               "evaluated."},
        {"static", "storage class", "At file scope: internal linkage, so the name is private to this file. "
                                    "Inside a function: the object outlives the call."},
        {"struct", "type", "An aggregate whose members are laid out in order, with padding between them."},
        {"switch", "statement", "Branches on an integer value to a `case` label."},
        {"typedef", "declaration", "Gives an existing type another name. It declares no object."},
        {"union", "type", "An aggregate whose members share the same storage. Only the member last written to "
                          "may be read, with the documented exception for a common initial sequence."},
        {"unsigned", "type", "Makes an integer type unsigned: no negative values, and arithmetic wraps rather "
                             "than overflowing."},
        {"void", "type", "No type: as a return type, no value; as `void *`, a pointer to anything; as a "
                         "parameter list, no parameters."},
        {"volatile", "qualifier", "Every read and write through this name must actually happen, in order. For "
                                  "hardware registers and `sig_atomic_t`, not for threads."},
        {"while", "statement", "Loops while the condition is non-zero, testing it first."},
        {"_Alignas", "specifier", "Sets an object's alignment. Spelled `alignas` with <stdalign.h>."},
        {"_Alignof", "operator", "The alignment a type requires. Spelled `alignof` with <stdalign.h>."},
        {"_Atomic", "qualifier", "Makes access to this object atomic and, by default, sequentially consistent."},
        {"_Bool", "type", "The boolean type: every non-zero value converted to it becomes 1. Spelled `bool` "
                          "with <stdbool.h>, and in C23 outright."},
        {"_Complex", "type", "A complex number of the given floating type. See <complex.h>."},
        {"_Generic", "operator", "Chooses an expression by the type of its controlling operand, at compile "
                                 "time. The machinery behind <tgmath.h>."},
        {"_Noreturn", "function specifier", "Promises the function never returns. Spelled `noreturn` with "
                                            "<stdnoreturn.h>; in C23, `[[noreturn]]`."},
        {"_Static_assert", "declaration", "Fails the compilation when its constant condition is false. Spelled "
                                          "`static_assert` with <assert.h>."},
        {"_Thread_local", "storage class", "One object per thread. Spelled `thread_local` with <threads.h>."},
        {"bool", "type", "The boolean type (`_Bool`), via <stdbool.h> or C23."},
        {"true", "constant", "The boolean true, `1`."},
        {"false", "constant", "The boolean false, `0`."},
        {"typeof", "operator", "The type of an expression, without evaluating it. A GNU extension until C23."},
        {"__attribute__", "GNU extension", "Attaches an attribute to a declaration: `((packed))`, "
                                           "`((unused))`, `((format(printf, 1, 2)))`, and the rest."},
        {"asm", "GNU extension", "Inline assembly. This server deliberately does not read its contents."},
    };
    return kVocab;
}

// The preprocessor directives, offered after a `#`.
const std::vector<CLspVocabEntry> &Directives() {
    static const std::vector<CLspVocabEntry> kVocab = {
        {"include", "#include <header.h>", "Pastes a header in at this point. Angle brackets search the system "
                                           "path; quotes search this file's directory first."},
        {"define", "#define NAME replacement", "Defines a macro. With a `(` touching the name, a function-like "
                                               "one -- the space matters."},
        {"undef", "#undef NAME", "Removes a macro definition. Harmless if it was not defined."},
        {"if", "#if condition", "Keeps the following lines when the constant condition is non-zero. An "
                                "identifier that is not a defined macro counts as 0."},
        {"ifdef", "#ifdef NAME", "Keeps the following lines when NAME is a defined macro."},
        {"ifndef", "#ifndef NAME", "Keeps the following lines when NAME is not defined. The header guard idiom."},
        {"elif", "#elif condition", "An `else if` for the preprocessor."},
        {"elifdef", "#elifdef NAME", "C23: an `#elif defined(NAME)`."},
        {"elifndef", "#elifndef NAME", "C23: an `#elif !defined(NAME)`."},
        {"else", "#else", "The branch taken when every condition above it was false."},
        {"endif", "#endif", "Closes the innermost `#if`, `#ifdef` or `#ifndef`."},
        {"error", "#error message", "Fails the compilation with a message."},
        {"warning", "#warning message", "Warns during compilation. Standard in C23, and supported long before."},
        {"pragma", "#pragma ...", "An implementation-defined instruction. `#pragma once` and `#pragma pack` "
                                  "are the two worth knowing."},
        {"line", "#line number \"file\"", "Renumbers the lines that follow, for generated code."},
        {"embed", "#embed \"file\"", "C23: pastes a file's bytes in as a comma-separated list."},
    };
    return kVocab;
}

// One sentence for the standard-library names worth a sentence. The
// signature beside each of these comes from the generated table, not
// from here, so this is only ever prose -- being wrong about it costs a
// misleading hover, never a wrong diagnostic.
const std::unordered_map<std::string, const char *> &Docs() {
    static const std::unordered_map<std::string, const char *> kDocs = {
        // <stdio.h>
        {"printf", "Writes a formatted string to stdout. Returns the number of characters written, or negative "
                   "on failure."},
        {"fprintf", "Writes a formatted string to a stream."},
        {"sprintf", "Writes a formatted string into a buffer, with no idea how big the buffer is. Prefer "
                    "`snprintf`."},
        {"snprintf", "Writes a formatted string into a buffer of at most `n` bytes, always NUL-terminating. "
                     "Returns the length it *would* have written, which can exceed `n`."},
        {"puts", "Writes a string and a newline to stdout."},
        {"fputs", "Writes a string to a stream, with no newline added."},
        {"fopen", "Opens a file. Returns NULL on failure, with the reason in `errno`."},
        {"fclose", "Closes a stream, flushing it first. Returns 0 on success."},
        {"fread", "Reads `nmemb` items of `size` bytes. Returns the number of *items* read, which is short at "
                  "end of file or on error."},
        {"fwrite", "Writes `nmemb` items of `size` bytes. Returns the number of items written."},
        {"fgets", "Reads at most `n - 1` bytes into a buffer, stopping after a newline, and NUL-terminates. "
                  "Returns NULL at end of file."},
        {"getline", "Reads a whole line, growing the buffer as needed. POSIX; free the buffer yourself."},
        {"fseek", "Moves a stream's position. Returns 0 on success."},
        {"ftell", "Reports a stream's position, or -1 on failure."},
        {"perror", "Writes a message and the text of the current `errno` to stderr."},
        {"scanf", "Reads formatted input from stdin. Returns the number of items assigned, which is how you "
                  "tell a partial parse from a complete one."},
        {"sscanf", "Reads formatted input from a string."},
        // <stdlib.h>
        {"malloc", "Allocates `size` bytes, uninitialized. Returns NULL on failure; `malloc(0)` may return "
                   "NULL or a unique pointer."},
        {"calloc", "Allocates `nmemb * size` bytes set to zero, and checks that multiplication for overflow."},
        {"realloc", "Resizes an allocation, moving it if it must. On failure it returns NULL and leaves the "
                    "old block alive, so never assign it straight back over your only pointer."},
        {"free", "Releases an allocation. Freeing NULL is defined and does nothing."},
        {"exit", "Ends the process, running `atexit` handlers and flushing streams."},
        {"abort", "Ends the process immediately, without flushing. Raises SIGABRT."},
        {"atoi", "Parses an int, with no way to report failure. Use `strtol` when the input might be bad."},
        {"strtol", "Parses a long in the given base. Sets `endptr` to the first unconsumed character and "
                   "`errno` to ERANGE on overflow, which together are how you check it."},
        {"strtoul", "Parses an unsigned long. Note that it accepts a leading `-` and wraps."},
        {"strtod", "Parses a double, reporting where it stopped through `endptr`."},
        {"qsort", "Sorts an array in place with a comparison function. Not a stable sort."},
        {"bsearch", "Binary-searches a sorted array. Returns NULL when there is no match."},
        {"getenv", "Reads an environment variable, or NULL. The string belongs to the environment; do not free "
                   "it."},
        {"rand", "A low-quality pseudo-random number. Fine for a shuffle, never for anything secret."},
        // <string.h>
        {"memcpy", "Copies `n` bytes. The regions must not overlap -- use `memmove` when they might."},
        {"memmove", "Copies `n` bytes, correctly even when the regions overlap."},
        {"memset", "Fills `n` bytes with a byte value. The value is converted to `unsigned char`, so "
                   "`memset(p, 1, n)` does not fill with the integer 1."},
        {"memcmp", "Compares `n` bytes. Not usable on structs with padding, whose padding bytes are "
                   "unspecified."},
        {"strlen", "The length of a string, not counting its NUL."},
        {"strcpy", "Copies a string including its NUL, with no bound. The caller must already know it fits."},
        {"strncpy", "Copies at most `n` bytes, and does *not* NUL-terminate when the source is that long. It "
                    "was designed for fixed-size records, not for strings."},
        {"strcat", "Appends a string. Walks the destination to find its end every time."},
        {"strcmp", "Compares two strings; returns negative, zero or positive. Not a boolean."},
        {"strncmp", "Compares at most `n` bytes of two strings."},
        {"strchr", "Finds a byte in a string, or NULL. Searching for '\\0' finds the terminator."},
        {"strrchr", "Finds the last occurrence of a byte in a string, or NULL."},
        {"strstr", "Finds a substring, or NULL."},
        {"strdup", "Copies a string into a fresh allocation. POSIX, and C23; free it yourself."},
        {"strtok", "Splits a string in place, keeping state between calls in a static variable. Not reentrant "
                   "and not thread-safe -- `strtok_r` is."},
        {"strerror", "The text of an `errno` value. Not thread-safe; `strerror_r` is."},
        // <ctype.h>
        {"isalpha", "Whether the byte is a letter. The argument must be representable as `unsigned char` or "
                    "EOF, so cast a plain `char` before passing it."},
        {"isdigit", "Whether the byte is a decimal digit."},
        {"isspace", "Whether the byte is whitespace: space, form feed, newline, carriage return, tab, vertical "
                    "tab."},
        {"tolower", "The lower-case form of a byte, or the byte unchanged."},
        {"toupper", "The upper-case form of a byte, or the byte unchanged."},
        // <assert.h>, <errno.h>
        {"assert", "Aborts when its condition is false. Compiled out entirely by NDEBUG, so never put anything "
                   "with a side effect inside one."},
        {"errno", "The last error a library call reported. Only meaningful right after a call that failed; "
                  "library calls may set it even when they succeed."},
        // <time.h>
        {"time", "The current calendar time, in seconds since the epoch."},
        {"clock", "Processor time used, in CLOCKS_PER_SEC units. Not wall-clock time."},
        {"strftime", "Formats a broken-down time. Returns 0 when the result would not fit, which is not "
                     "distinguishable from an empty format."},
        {"localtime", "Converts a `time_t` to local broken-down time, into a static buffer. `localtime_r` is "
                      "the reentrant one."},
        // POSIX
        {"open", "Opens a file descriptor. Returns -1 on failure with the reason in `errno`."},
        {"read", "Reads up to `count` bytes. A short read is normal, not an error; 0 means end of file."},
        {"write", "Writes up to `count` bytes. A short write is normal on pipes and sockets -- loop."},
        {"close", "Closes a file descriptor. Check the result: a deferred write error surfaces here."},
        {"fork", "Creates a child process. Returns 0 in the child and the child's pid in the parent."},
        {"execvp", "Replaces this process's image, searching PATH. It only returns when it failed."},
        {"waitpid", "Waits for a child to change state."},
        {"pipe", "Creates a pipe: `fds[0]` reads, `fds[1]` writes."},
        {"dup2", "Duplicates a file descriptor onto a specific number, closing whatever was there."},
        {"mmap", "Maps a file or anonymous memory. Returns MAP_FAILED, not NULL, on failure."},
        {"socket", "Creates an endpoint for communication, as a file descriptor."},
        {"poll", "Waits for any of a set of file descriptors to be ready."},
        {"pthread_create", "Starts a thread. The start routine takes and returns `void *`."},
        {"pthread_join", "Waits for a thread to finish and collects its return value."},
        {"pthread_mutex_lock", "Locks a mutex, blocking until it can. Relocking one you already hold is "
                               "undefined unless it is recursive."},
        // <stddef.h> and friends
        {"NULL", "The null pointer constant. In C, writing `0` means the same thing."},
        {"size_t", "The unsigned type a `sizeof` yields, and the right type for a count of bytes or elements. "
                   "It never goes below zero, which is what makes `for (size_t i = n - 1; i >= 0; i--)` loop "
                   "forever."},
        {"ssize_t", "A signed size, used by POSIX calls that return either a count or -1."},
        {"ptrdiff_t", "The signed type a pointer subtraction yields."},
        {"offsetof", "The byte offset of a member within a struct."},
        {"va_start", "Begins access to a variadic function's extra arguments. Pair it with `va_end`."},
        {"va_arg", "Reads the next variadic argument, given its type. Nothing checks that the type is right."},
        {"va_end", "Ends access to variadic arguments. Required, even where it does nothing."},
        {"EOF", "The value the character-input functions return at end of file. It is negative, which is why "
                "they return `int` rather than `char`."},
    };
    return kDocs;
}

/** @brief Builds, once, the per-header member lists out of the generated declaration table. */
const std::unordered_map<std::string, std::vector<CLspVocabEntry>> &HeaderMembers() {
    static const std::unordered_map<std::string, std::vector<CLspVocabEntry>> kMap = [] {
        std::unordered_map<std::string, std::vector<CLspVocabEntry>> map;
        for (size_t i = 0; i < kCStdDeclCount; i++) {
            const CStdDecl &decl = kCStdDecls[i];
            CLspVocabEntry entry;
            entry.name = decl.name;
            entry.detail = decl.detail;
            const auto doc = Docs().find(decl.name);
            entry.doc = doc == Docs().end() ? "" : doc->second;
            map[decl.header].push_back(entry);
        }
        return map;
    }();
    return kMap;
}

}  // namespace

const std::vector<CLspVocabEntry> &CLspKeywordVocab() { return Keywords(); }

const std::vector<CLspVocabEntry> &CLspDirectiveVocab() { return Directives(); }

const std::vector<CLspVocabEntry> &CLspHeaderVocab() {
    static const std::vector<CLspVocabEntry> kVocab = [] {
        std::vector<CLspVocabEntry> out;
        for (size_t i = 0; i < kCStdHeaderCount; i++) {
            CLspVocabEntry entry;
            entry.name = kCStdHeaders[i];
            entry.detail = "";
            entry.doc = "";
            out.push_back(entry);
        }
        return out;
    }();
    return kVocab;
}

const std::vector<CLspVocabEntry> *CLspHeaderMembers(const std::string &header) {
    const auto it = HeaderMembers().find(header);
    return it == HeaderMembers().end() ? nullptr : &it->second;
}

bool CLspKnowsHeader(const std::string &header) {
    for (size_t i = 0; i < kCStdHeaderCount; i++) {
        if (header == kCStdHeaders[i]) return true;
    }
    return false;
}

const CLspVocabEntry *CLspLookupStandardName(const std::string &name) {
    // One entry per name, built on first use, so hover and completion
    // both get the same answer and neither pays for a scan.
    static const std::unordered_map<std::string, CLspVocabEntry> kByName = [] {
        std::unordered_map<std::string, CLspVocabEntry> map;
        for (size_t i = 0; i < kCStdDeclCount; i++) {
            const CStdDecl &decl = kCStdDecls[i];
            CLspVocabEntry entry;
            entry.name = decl.name;
            entry.detail = decl.detail;
            const auto doc = Docs().find(decl.name);
            entry.doc = doc == Docs().end() ? "" : doc->second;
            map.emplace(decl.name, entry);
        }
        return map;
    }();
    const auto it = kByName.find(name);
    return it == kByName.end() ? nullptr : &it->second;
}

std::string CLspHeaderOf(const std::string &name) {
    size_t lo = 0;
    size_t hi = kCStdDeclCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const int cmp = std::string(kCStdDecls[mid].name).compare(name);
        if (cmp == 0) return kCStdDecls[mid].header;
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return std::string();
}

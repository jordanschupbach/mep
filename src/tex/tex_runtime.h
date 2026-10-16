#ifndef MEP_TEX_RUNTIME_H
#define MEP_TEX_RUNTIME_H

// The hand-written ground the generated engine (src/tex/gen/, made from
// tex.web by tools/web2cpp) stands on: what tex.web's Pascal took from its
// compiler and operating system. The memory word and its parts, arrays
// with Pascal bounds, Pascal's three kinds of file, and the handful of
// Pascal built-ins the translation maps to functions.
//
// Everything here is shaped by what the translated code does with it, so
// each piece keeps Pascal's meaning rather than C++'s: a file's buffer
// variable (`f^`) holds the element at the current position, `round`
// rounds halves away from zero, an array is indexed from its own lower
// bound.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "tex/tex_pdf_state.h"

namespace meptex {

// --- The memory word ---------------------------------------------------------------
//
// tex.web's variant records, laid out as web2c lays them out: eight bytes,
// a halfword `rh` sharing its word with either another halfword `lh` or two
// quarterwords `b0`/`b1`; four quarterwords; one integer; one glue ratio
// (a double, as in TeX Live, so glue is set to the same rounding).

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"  // anonymous structs, as in C11
#endif
struct two_halves {
    int32_t rh;
    union {
        int32_t lh;
        struct {
            uint16_t b0;
            uint16_t b1;
        };
    };
};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

struct four_quarters {
    uint16_t b0;
    uint16_t b1;
    uint16_t b2;
    uint16_t b3;
};

union memory_word {
    int32_t int_;  // Pascal's `int` field (also `sc`, a scaled value)
    double gr;
    two_halves hh;
    four_quarters qqqq;
};

static_assert(sizeof(memory_word) == 8, "a memory word is eight bytes");

// --- Arrays ----------------------------------------------------------------------------

/**
 * @brief A Pascal array: indexed from its declared lower bound, sized when the engine starts.
 */
template <class T>
class PArray {
public:
    /** @brief Gives the array the index range lo..hi (inclusive), zero-filled. */
    void alloc(int64_t lo, int64_t hi) {
        lo_ = lo;
        n_ = hi >= lo ? static_cast<size_t>(hi - lo + 1) : 0;
        data_.reset(new T[n_ > 0 ? n_ : 1]());
    }
    T &operator[](int64_t i) { return data_[static_cast<size_t>(i - lo_)]; }
    const T &operator[](int64_t i) const { return data_[static_cast<size_t>(i - lo_)]; }
    int64_t lo() const { return lo_; }
    size_t size() const { return n_; }
    T *data() { return data_.get(); }

private:
    std::unique_ptr<T[]> data_;
    int64_t lo_ = 0;
    size_t n_ = 0;
};

// --- Files -------------------------------------------------------------------------------

/**
 * @brief A Pascal text file: the terminal, the log, \input and \openin files, \write streams.
 */
class AlphaFile {
public:
    AlphaFile() = default;
    AlphaFile(const AlphaFile &) = delete;
    AlphaFile &operator=(const AlphaFile &) = delete;
    ~AlphaFile();

    bool open_read(const std::string &path);
    bool open_write(const std::string &path);
    /** @brief Reads from a string instead of a file (tests). */
    void open_string(std::string text);
    /** @brief Reads the given lines, each exactly as given (a \scantokens pseudo-file). */
    void open_lines(std::vector<std::string> lines);
    void open_stdin();
    void open_stdout();
    void close();
    bool is_open() const { return f_ != nullptr || from_string_ || from_lines_; }

    /** @brief Whether nothing at all is left to read. */
    bool at_eof();
    /**
     * @brief The next character of the current line, or -1 at its end (the line
     * terminator -- LF, CR LF or CR -- is consumed; end of file also ends a line).
     */
    int next_char();

    void write(const char *s);
    void write(const std::string &s) { write(s.c_str()); }
    void write(uint8_t c);
    void write(int32_t n);
    void write(double d);
    void write_ln();
    void flush();
    /** @brief Mirror everything written to this file into `sink` too (an in-process log). */
    void set_sink(std::string *sink) { sink_ = sink; }

private:
    int getc_();
    void ungetc_(int c);
    FILE *f_ = nullptr;
    bool owns_ = false;
    bool from_string_ = false;
    std::string text_;
    size_t pos_ = 0;
    bool from_lines_ = false;
    std::vector<std::string> lines_;
    size_t line_ = 0;
    std::string *sink_ = nullptr;
};

/**
 * @brief A Pascal file of bytes: font metric files (read) and the DVI file (written).
 */
class ByteFile {
public:
    ByteFile() = default;
    ByteFile(const ByteFile &) = delete;
    ByteFile &operator=(const ByteFile &) = delete;
    ~ByteFile();

    bool open_read(const std::string &path);
    bool open_write(const std::string &path);
    void close();
    /** @brief Pascal's `f^`: the byte at the current position (0 at the end). */
    uint8_t &buf() { return cur_; }
    void get();
    bool eof() const { return pos_ >= data_.size(); }
    void write(int32_t byte);
    void write(uint8_t byte) { write(static_cast<int32_t>(byte)); }

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
    uint8_t cur_ = 0;
    FILE *out_ = nullptr;
};

/**
 * @brief A Pascal file of memory words: the format file, dumped and undumped.
 */
class WordFile {
public:
    WordFile() = default;
    WordFile(const WordFile &) = delete;
    WordFile &operator=(const WordFile &) = delete;
    ~WordFile();

    bool open_read(const std::string &path);
    bool open_write(const std::string &path);
    void close();
    /** @brief Pascal's `f^`: on input the current word, on output the word `put` writes next. */
    memory_word &buf() { return cur_; }
    void get();
    void put();
    bool eof() const { return pos_ >= words_.size(); }

private:
    std::vector<memory_word> words_;
    size_t pos_ = 0;
    memory_word cur_{};
    std::vector<memory_word> pending_;
    FILE *out_ = nullptr;
};

/** @brief Pascal's assignment of a string constant to a `packed array of char`. */
template <class T>
void pset(PArray<T> &a, const char *s) {
    for (size_t i = 0; s[i] != 0 && i < a.size(); i++) a[a.lo() + static_cast<int64_t>(i)] = static_cast<T>(s[i]);
}

template <class F, class... A>
void rt_write(F &f, A... a) {
    (f.write(a), ...);
}
template <class F, class... A>
void rt_write_ln(F &f, A... a) {
    (f.write(a), ...);
    f.write_ln();
}

// --- Pascal built-ins ------------------------------------------------------------------

inline int32_t pabs(int32_t x) { return x < 0 ? -x : x; }
inline double pabs(double x) { return std::fabs(x); }
inline bool podd(int32_t x) { return (x & 1) != 0; }
/** @brief Pascal's round: halves away from zero. */
inline int32_t pround(double x) { return static_cast<int32_t>(std::lround(x)); }

/** @brief A goto to a label of the main program from inside a routine (`goto final_end`). */
struct TexJump {
    int32_t label;
};

// The pool strings the translation collected (tex_pool.cpp), numbered from 256.
extern const int kPoolStringCount;
extern const char *const kPoolStrings[];

class Resolver;

/**
 * @brief What the engine is asked to do: set by the caller before Engine::run.
 */
struct EngineOptions {
    bool ini = false;                 // INITEX: build tables (and \dump a format) from scratch
    std::string first_line;           // the command line: TeX's first line of input
    std::string output_dir;           // where the log, DVI/PDF and \openout files go ("" = cwd)
    std::vector<std::string> format_dirs;  // where .fmt files are looked for and dumped
    std::string format;               // the format to load (as `&name` on the first line would), "" = none
    bool printable_8bit = true;       // cp227.tcx: 128-255 and HT/LF/VT print as themselves
    bool quiet = false;               // print nothing on the terminal (the log still has it all)
    int interaction = -1;             // -1: as the format says; else batch(0)..errorstop(3)
    // Capacities (0 = default).
    int64_t main_memory = 0;
    int64_t pool_size = 0;
    int64_t max_strings = 0;
    int64_t font_mem_size = 0;
    int64_t buf_size = 0;
    int64_t save_size = 0;
    int64_t stack_size = 0;
    int64_t trie_size = 0;
    int64_t error_line = 0, half_error_line = 0, max_print_line = 0;
    int64_t source_date_epoch = -1;   // fixed date and time, as SOURCE_DATE_EPOCH (-1 = now)
};

/**
 * @brief The state the generated Engine inherits: options and the file resolver.
 */
struct EngineBase {
    EngineOptions opt;
    Resolver *resolver = nullptr;     // not owned
    std::string terminal_log;         // everything printed on the terminal, when captured
};

}  // namespace meptex

#endif

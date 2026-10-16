#include "tex/tex_runtime.h"

#include <cinttypes>
#include <cstring>

namespace meptex {

// --- AlphaFile -----------------------------------------------------------------------

AlphaFile::~AlphaFile() { close(); }

bool AlphaFile::open_read(const std::string &path) {
    close();
    f_ = std::fopen(path.c_str(), "rb");
    owns_ = f_ != nullptr;
    return f_ != nullptr;
}

bool AlphaFile::open_write(const std::string &path) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    owns_ = f_ != nullptr;
    return f_ != nullptr;
}

void AlphaFile::open_string(std::string text) {
    close();
    text_ = std::move(text);
    pos_ = 0;
    from_string_ = true;
}

void AlphaFile::open_lines(std::vector<std::string> lines) {
    close();
    lines_ = std::move(lines);
    line_ = 0;
    pos_ = 0;
    from_lines_ = true;
}

void AlphaFile::open_stdin() {
    close();
    f_ = stdin;
    owns_ = false;
}

void AlphaFile::open_stdout() {
    close();
    f_ = stdout;
    owns_ = false;
}

void AlphaFile::close() {
    if (f_ && owns_) std::fclose(f_);
    f_ = nullptr;
    owns_ = false;
    from_string_ = false;
    text_.clear();
    pos_ = 0;
    from_lines_ = false;
    lines_.clear();
    line_ = 0;
}

int AlphaFile::getc_() {
    if (from_string_) return pos_ < text_.size() ? static_cast<unsigned char>(text_[pos_++]) : EOF;
    return f_ ? std::getc(f_) : EOF;
}

void AlphaFile::ungetc_(int c) {
    if (c == EOF) return;
    if (from_string_) {
        if (pos_ > 0) --pos_;
        return;
    }
    if (f_) std::ungetc(c, f_);
}

bool AlphaFile::at_eof() {
    if (from_lines_) return line_ >= lines_.size();
    const int c = getc_();
    if (c == EOF) return true;
    ungetc_(c);
    return false;
}

int AlphaFile::next_char() {
    if (from_lines_) {
        if (line_ >= lines_.size()) return -1;
        const std::string &l = lines_[line_];
        if (pos_ < l.size()) return static_cast<unsigned char>(l[pos_++]);
        ++line_;
        pos_ = 0;
        return -1;
    }
    const int c = getc_();
    if (c == EOF || c == '\n') return -1;
    if (c == '\r') {
        const int d = getc_();
        if (d != '\n') ungetc_(d);
        return -1;
    }
    return c;
}

void AlphaFile::write(const char *s) {
    if (sink_) sink_->append(s);
    if (f_) std::fputs(s, f_);
}

void AlphaFile::write(uint8_t c) {
    if (sink_) sink_->push_back(static_cast<char>(c));
    if (f_) std::fputc(c, f_);
}

void AlphaFile::write(int32_t n) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%" PRId32, n);
    write(buf);
}

void AlphaFile::write(double d) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", d);
    write(buf);
}

void AlphaFile::write_ln() {
    if (sink_) sink_->push_back('\n');
    if (f_) std::fputc('\n', f_);
}

void AlphaFile::flush() {
    if (f_) std::fflush(f_);
}

// --- ByteFile ---------------------------------------------------------------------------

ByteFile::~ByteFile() { close(); }

bool ByteFile::open_read(const std::string &path) {
    close();
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) data_.insert(data_.end(), chunk, chunk + n);
    std::fclose(f);
    pos_ = 0;
    cur_ = data_.empty() ? 0 : data_[0];
    return true;
}

bool ByteFile::open_write(const std::string &path) {
    close();
    out_ = std::fopen(path.c_str(), "wb");
    return out_ != nullptr;
}

void ByteFile::close() {
    if (out_) std::fclose(out_);
    out_ = nullptr;
    data_.clear();
    pos_ = 0;
    cur_ = 0;
}

void ByteFile::get() {
    if (pos_ < data_.size()) ++pos_;
    cur_ = pos_ < data_.size() ? data_[pos_] : 0;
}

void ByteFile::write(int32_t byte) {
    if (out_) std::fputc(byte & 0xff, out_);
}

// --- WordFile ---------------------------------------------------------------------------

WordFile::~WordFile() { close(); }

bool WordFile::open_read(const std::string &path) {
    close();
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    memory_word chunk[8192];
    size_t n;
    while ((n = std::fread(chunk, sizeof(memory_word), 8192, f)) > 0) words_.insert(words_.end(), chunk, chunk + n);
    std::fclose(f);
    pos_ = 0;
    cur_ = words_.empty() ? memory_word{} : words_[0];
    return true;
}

bool WordFile::open_write(const std::string &path) {
    close();
    out_ = std::fopen(path.c_str(), "wb");
    return out_ != nullptr;
}

void WordFile::close() {
    if (out_) {
        if (!pending_.empty()) std::fwrite(pending_.data(), sizeof(memory_word), pending_.size(), out_);
        std::fclose(out_);
    }
    out_ = nullptr;
    pending_.clear();
    words_.clear();
    pos_ = 0;
}

void WordFile::get() {
    if (pos_ < words_.size()) ++pos_;
    cur_ = pos_ < words_.size() ? words_[pos_] : memory_word{};
}

void WordFile::put() {
    pending_.push_back(cur_);
    if (pending_.size() >= 65536) {
        std::fwrite(pending_.data(), sizeof(memory_word), pending_.size(), out_);
        pending_.clear();
    }
}

}  // namespace meptex

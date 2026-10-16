// The hand-written members of meptex::Engine (tex_engine_members.h): what
// mep-tex.ch asks of the system that tex.web left to the Pascal run-time
// -- capacities, opening files by name through the resolver, the command
// line, TeX's own strings, the clock -- and the entry point that runs the
// translated program.

#include "tex/gen/tex_gen.h"
#include "tex/tex_resolver.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace meptex {

namespace {

int32_t Cap(int64_t requested, int32_t dflt) {
    return requested > 0 ? static_cast<int32_t>(requested) : dflt;
}

bool IsAbsolute(const std::string &p) { return !p.empty() && p[0] == '/'; }

}  // namespace

void Engine::set_capacities() {
    // TeX Live's texmf.cnf values, so a document that fits there fits here.
    mem_min = 0;  // mem_bot
    mem_top = Cap(opt.main_memory, 5000000) - 1;
    mem_max = mem_top;
    buf_size = Cap(opt.buf_size, 200000);
    error_line = Cap(opt.error_line, 79);
    half_error_line = Cap(opt.half_error_line, 50);
    max_print_line = Cap(opt.max_print_line, 79);
    stack_size = Cap(opt.stack_size, 10000);
    max_in_open = 15;
    font_mem_size = Cap(opt.font_mem_size, 8000000);
    param_size = 10000;
    nest_size = 500;
    max_strings = Cap(opt.max_strings, 500000);
    string_vacancies = 90000;
    pool_size = Cap(opt.pool_size, 6250000);
    save_size = Cap(opt.save_size, 100000);
    trie_size = Cap(opt.trie_size, 1000000);
    dvi_buf_size = 16384;
    file_name_size = 4096;
    ini_version = opt.ini;
}

int Engine::run() {
    set_capacities();
    alloc_arrays();
    for (int k = 0; k < 256; k++) {
        const bool standard = k >= ' ' && k <= '~';
        const bool cp227 = k >= 128 || k == 9 || k == 10 || k == 11;
        printable[k] = standard || (opt.printable_8bit && cp227);
    }
    main_body();
    if (term_offset > 0) term_out.write_ln();  // the last line ends, as TeX Live's does
    term_out.flush();
    return history <= 1 ? 0 : 1;  // spotless or warning_issued
}

void Engine::open_terminal() {
    if (opt.quiet) term_out.open_write("/dev/null");
    else term_out.open_stdout();
}

void Engine::apply_option_interaction() {
    if (opt.interaction >= tc::batch_mode && opt.interaction <= tc::error_stop_mode) interaction = opt.interaction;
}

bool Engine::open_option_fmt() {
    if (opt.format.empty()) return false;
    set_current_file_name(opt.format + ".fmt");
    return w_open_in(fmt_file);
}

std::string Engine::current_file_name() const {
    std::string s;
    s.reserve(static_cast<size_t>(name_length));
    for (int32_t k = 1; k <= name_length; k++) s.push_back(static_cast<char>(name_of_file[k]));
    return s;
}

void Engine::set_current_file_name(const std::string &name) {
    const int32_t n = static_cast<int32_t>(std::min<size_t>(name.size(), static_cast<size_t>(file_name_size)));
    for (int32_t k = 1; k <= n; k++) name_of_file[k] = static_cast<uint8_t>(name[static_cast<size_t>(k - 1)]);
    for (int32_t k = n + 1; k <= file_name_size && k <= name_length; k++) name_of_file[k] = ' ';
    name_length = n;
}

bool Engine::open_in_file(AlphaFile &file, int32_t kind) {
    const std::string path = resolver ? resolver->find(current_file_name(), kind) : current_file_name();
    if (path.empty() || !file.open_read(path)) return false;
    set_current_file_name(path);
    return true;
}

bool Engine::open_in_file(ByteFile &file, int32_t kind) {
    const std::string path = resolver ? resolver->find(current_file_name(), kind) : current_file_name();
    if (path.empty() || !file.open_read(path)) return false;
    set_current_file_name(path);
    return true;
}

bool Engine::open_in_file(WordFile &file, int32_t kind) {
    std::string name = current_file_name();
    std::string path;
    for (const std::string &d : opt.format_dirs) {
        const std::string p = d + "/" + name;
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            path = p;
            break;
        }
    }
    if (path.empty() && resolver) path = resolver->find(name, kind);
    if (path.empty()) path = name;
    if (!file.open_read(path)) return false;
    set_current_file_name(path);
    return true;
}

namespace {
std::string OutPath(const EngineOptions &opt, const std::string &name, bool is_format) {
    if (IsAbsolute(name)) return name;
    if (is_format && !opt.format_dirs.empty() && opt.format_dirs.front() != ".")
        return opt.format_dirs.front() + "/" + name;
    if (!opt.output_dir.empty()) return opt.output_dir + "/" + name;
    return name;
}
}  // namespace

bool Engine::open_out_file(AlphaFile &file) {
    const std::string path = OutPath(opt, current_file_name(), false);
    if (!file.open_write(path)) return false;
    set_current_file_name(path);
    return true;
}

bool Engine::open_out_file(ByteFile &file) {
    const std::string path = OutPath(opt, current_file_name(), false);
    if (!file.open_write(path)) return false;
    set_current_file_name(path);
    return true;
}

bool Engine::open_out_file(WordFile &file) {
    const std::string path = OutPath(opt, current_file_name(), true);
    if (!file.open_write(path)) return false;
    set_current_file_name(path);
    return true;
}

void Engine::load_command_line() {
    last = first;
    for (char ch : opt.first_line) {
        if (last + 1 >= buf_size) break;
        buffer[last++] = xord[static_cast<unsigned char>(ch)];
    }
    while (last > first && buffer[last - 1] == ' ') --last;
}

bool Engine::load_pool_strings() {
    for (int i = 0; i < kPoolStringCount; i++) {
        const char *s = kPoolStrings[i];
        const size_t n = std::strlen(s);
        if (static_cast<size_t>(pool_ptr) + n + static_cast<size_t>(string_vacancies) >
            static_cast<size_t>(pool_size)) {
            term_out.write("! You have to increase POOLSIZE.");
            term_out.write_ln();
            return false;
        }
        for (size_t j = 0; j < n; j++) str_pool[pool_ptr++] = static_cast<uint8_t>(s[j]);
        make_string();
    }
    return true;
}

void Engine::system_date_and_time() {
    std::time_t now = std::time(nullptr);
    int64_t epoch = opt.source_date_epoch;
    if (epoch < 0) {
        // As in TeX Live: SOURCE_DATE_EPOCH sets \time, \day, \month and
        // \year only when FORCE_SOURCE_DATE=1 asks for it.
        const char *force = std::getenv("FORCE_SOURCE_DATE");
        const char *e = std::getenv("SOURCE_DATE_EPOCH");
        if (force && std::strcmp(force, "1") == 0 && e && *e) epoch = std::atoll(e);
    }
    std::tm tmv{};
    if (epoch >= 0) {
        now = static_cast<std::time_t>(epoch);
        gmtime_r(&now, &tmv);
    } else {
        localtime_r(&now, &tmv);
    }
    sys_time = tmv.tm_hour * 60 + tmv.tm_min;
    sys_day = tmv.tm_mday;
    sys_month = tmv.tm_mon + 1;
    sys_year = tmv.tm_year + 1900;
}

}  // namespace meptex

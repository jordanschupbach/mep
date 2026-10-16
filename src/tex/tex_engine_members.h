// Hand-written members of meptex::Engine, #included inside the generated
// class body (src/tex/gen/tex_gen.h): the routines mep-tex.ch calls that
// are not in tex.web -- opening files through the resolver, the command
// line, the string pool, the clock -- and the entry point. Implemented in
// src/tex/tex_system.cpp, where they can reach every generated member.
// Deliberately no include guard and no namespace: it is part of a class.

#include "tex/tex_accessors.h"

/** @brief Sets the capacities (mem_top, pool_size, ...) from `opt`, before alloc_arrays. */
void set_capacities();
/** @brief Runs the job: capacities, arrays, then tex.web's main program. Returns an exit status. */
int run();

/** @brief Opens name_of_file for reading as a file of `kind` (tex_kind, tfm_kind, fmt_kind). */
bool open_in_file(AlphaFile &file, int32_t kind);
bool open_in_file(ByteFile &file, int32_t kind);
bool open_in_file(WordFile &file, int32_t kind);
/** @brief Opens name_of_file for writing, in the output directory. */
bool open_out_file(AlphaFile &file);
bool open_out_file(ByteFile &file);
bool open_out_file(WordFile &file);

/** @brief Opens the terminal: stdout, or nowhere when opt.quiet. */
void open_terminal();
/** @brief interaction := opt.interaction, when the caller gave one. */
void apply_option_interaction();
/** @brief Opens opt.format's .fmt file, when the caller named one. */
bool open_option_fmt();
/** @brief Puts the command line (opt.first_line) into buffer[first..last). */
void load_command_line();
/** @brief Makes TeX's own strings (kPoolStrings), as reading TEX.POOL did. */
bool load_pool_strings();
/** @brief Sets sys_time, sys_day, sys_month, sys_year from the clock (or opt.source_date_epoch). */
void system_date_and_time();

/** @brief name_of_file[1..name_length] as a string. */
std::string current_file_name() const;
/** @brief Sets name_of_file and name_length (truncated to file_name_size). */
void set_current_file_name(const std::string &name);

// --- e-TeX (tex_etex.cpp) ---------------------------------------------------------
void print_str(const char *s);
void print_esc_str(const char *s);
void print_nl_str(const char *s);
void print_err_str(const char *s);
/** @brief A pool string holding `s` (made once, then reused), for help lines written in C++. */
int32_t pool_string(const char *s);
std::unordered_map<std::string, int32_t> pool_string_cache_;
/** @brief Sets TeX's help lines, top line first. */
void set_help(std::initializer_list<const char *> lines);
void print_group(bool leaving);
void group_trace(bool leaving);
void show_save_groups();
void print_if_line(int32_t l);
void show_if_status();
void show_if_list();
void print_etex_last_item(int32_t chr);
void return_owned_glue(int32_t q, int32_t level);
void fetch_etex_item(int32_t m);
int32_t fetch_penalties(int32_t m);
void expr_next();
bool expr_is(int chr) const;
int32_t expr_check(int64_t v, int32_t l);
int32_t expr_quantity(int32_t l);
int32_t expr_factor(int32_t l);
int32_t expr_scale(int32_t l, int32_t v, int64_t n, int64_t d);
int32_t expr_term(int32_t l);
int32_t expr_add(int32_t l, int32_t a, int32_t b, bool subtract);
int32_t expr_sum(int32_t l);
/** @brief Scans an e-TeX expression of type `l` (int_val..mu_val); glue results are the caller's. */
int32_t scan_expr(int32_t l);
/** @brief \scantokens: reads a balanced text and starts reading it as a pseudo-file. */
void pseudo_start();
// Mark classes 1..32767 (\marks): top, first, bot, split first, split bot
// for each class, five per class, made on first use.
std::vector<int32_t> class_marks_;
int32_t max_mark_class_ = 0;
int32_t &class_mark_slot(int32_t cls, int32_t kind);
int32_t class_mark(int32_t cls, int32_t kind);
void vsplit_marks_init();
void vsplit_class_mark(int32_t p);
void fire_up_marks_init();
void page_class_mark(int32_t p);
void fire_up_marks_done();
void class_marks_cleanup();

// --- pdfTeX (tex_pdf.cpp, tex_pdf_out.cpp) ------------------------------------------------
PdfState pdf_;
/** @brief A token list shown as text (as \showtokens would print it). */
std::string tokens_to_string(int32_t list);
/** @brief Scans a <general text> (expanded or not) and returns it as text. */
/** @brief scan_toks(false, expand), returning the list and leaving def_ref as it was. */
int32_t scan_toks_list(bool expand);
std::string scan_pdf_text(bool expand);
/** @brief Puts `s` back into the input as character tokens (as \jobname's result is). */
void insert_string(const std::string &s);
bool scan_kw(const char *kw);
/** @brief A file a pdfTeX primitive names, found as TeX finds \\input files (then in the font trees); "" if none. */
std::string find_input_file(const std::string &name);
void scan_pdf_rule_spec(int32_t &w, int32_t &h, int32_t &d);
int32_t pdf_new_obj(int32_t kind);
PdfObject *pdf_obj(int32_t n);
/** @brief Appends a pdfTeX whatsit of subtype `sub` to the current list, with fresh data. */
int32_t new_pdf_node(int32_t sub);
PdfNodeData &pdf_node(int32_t p);
int32_t primitive_twin(int32_t cs, bool make);
void store_primitive(int32_t cs);
void pdf_primitive_expand();
bool pdf_if_primitive();
int32_t font_char_code(int32_t code, int32_t fnt, int32_t chr);
void set_font_char_code(int32_t code, int32_t fnt, int32_t chr, int32_t v);
void print_pdf_convert(int32_t chr);
void print_pdf_last_item(int32_t chr);
void print_pdf_extension(int32_t chr);
void pdf_random_init();
int32_t pdf_uniform(int32_t n);
int32_t pdf_normal();
double pdf_clock();
void fetch_pdf_item(int32_t m);
void pdf_convert(int32_t c);
void scan_pdf_action(PdfNodeData &d);
void do_pdf_extension(int32_t chr);
void pdf_scan_obj(bool immediate);
void pdf_scan_xform(bool immediate);
void pdf_scan_ximage(bool immediate);
bool pdf_immediate_ok(int32_t chr);
void pdf_immediate(int32_t chr);
void display_pdf_whatsit(int32_t p);
int32_t copy_pdf_whatsit(int32_t p, int32_t &words);
void free_pdf_whatsit(int32_t p);
void pdf_out_what_dvi(int32_t p);
int32_t cur_page_height_dvi();
/** @brief \input{name}: the braced file name, spaces and all. */
void scan_braced_file_name();
// The PDF writer (tex_pdf_out.cpp)
void pdf_ship_out(int32_t p);
void pdf_write_obj(int32_t n);
void pdf_read_image_size(PdfObject &o);
void pdf_write_image(int32_t n);
int32_t pdf_font_resource_number(int32_t f);
int32_t pdf_font_object(int32_t f);
int32_t pdf_page_object(int32_t page);
std::string pool_to_string(int32_t s);
/** @brief The PdfFont (index into pdf_.fonts) that draws TeX font `f`, made on first use. */
int32_t pdf_font_index(int32_t f);
double pdf_char_width(int32_t pf, int32_t c);
void pdf_out_char(PdfContent &c, int32_t f, int32_t ch, int32_t h, int32_t v);
void pdf_out_rule(PdfContent &c, int32_t h, int32_t v, int32_t w, int32_t ht);
void pdf_out_literal(PdfContent &c, const std::string &text, int32_t mode, int32_t h, int32_t v);
void pdf_colorstack_emit(PdfContent &c, int32_t stack, int32_t h, int32_t v);
void pdf_out_whatsit(PdfContent &c, int32_t p, int32_t h, int32_t v, int32_t this_box);
/** @brief A \special: dvipdfmx's pdf: commands (obj, put, code, literal, content, docinfo, docview). */
void pdf_special(PdfContent &c, const std::string &special, int32_t h, int32_t v);
int32_t pdf_special_name(const std::string &name);
/** @brief A special's text with each @name (outside strings) as a reference. */
std::string pdf_special_refs(const std::string &text);
/** @brief An action spec (as scan_pdf_action recorded it) as a PDF `/A` entry. */
std::string pdf_action(const std::string &a);
/** @brief hlist_out and vlist_out for PDF: the box's contents into `c`, at TeX's (h, v). */
void pdf_hlist_out(PdfContent &c, int32_t this_box, int32_t &cur_h_, int32_t &cur_v_);
void pdf_vlist_out(PdfContent &c, int32_t this_box, int32_t &cur_h_, int32_t &cur_v_);
void pdf_write_link_part(PdfContent &c, size_t k, int32_t right);
std::string pdf_expand_at_shipout(int32_t toks);
int32_t pdf_vf(int32_t fnt);
// tex_microtype.cpp: character protrusion
bool margin_skippable(int32_t p);
int32_t margin_protrusion(int32_t p, int32_t side);
int32_t margin_first_item(int32_t p, bool top);
int32_t margin_last_item(int32_t l, int32_t r, bool top);
int32_t margin_line_start(int32_t l, bool discard);
int32_t line_protrusion(int32_t pass, int32_t brk);
int32_t new_margin_kern(int32_t w, int32_t side);
int32_t protrude_right(int32_t q, bool after);
int32_t protrude_left(int32_t q);
int32_t margin_kern_of_box(int32_t b, bool right);
// tex_microtype.cpp: font expansion
int32_t font_base_of(int32_t fnt);
int32_t font_expansion_of(int32_t fnt);
int32_t font_char_code_of_base(int32_t code, int32_t fnt, int32_t chr);
int32_t char_expand_amount(int32_t fnt, int32_t chr, bool stretching);
int32_t make_font_copy(int32_t base, int32_t e, int32_t ls, bool own_params);
int32_t expanded_font(int32_t base, int32_t e);
int32_t font_cs_text(int32_t u);
void new_font_copy(bool letterspace, bool global);
int32_t pdf_font_root(int32_t fnt);
int32_t font_letterspace(int32_t fnt);
int32_t char_stretch(int32_t fnt, int32_t chr);
int32_t char_shrink(int32_t fnt, int32_t chr);
int32_t expansion_shortfall(int32_t shortfall, int32_t fstretch, int32_t fshrink);
void print_font_expansion(int32_t fnt);
void expand_line(int32_t q, int32_t w);
int32_t pdf_vf_local_font(const std::string &name, int32_t size);
void pdf_out_vf_char(PdfContent &c, int32_t vf, int32_t fnt, int32_t ch, int32_t h, int32_t v);
void pdf_begin_file();
void pdf_begin_obj(int32_t n);
void pdf_write_object_text(int32_t n, const std::string &body);
void pdf_write_stream(int32_t n, const std::string &dict, const std::string &data, bool compress);
bool pdf_writing();
std::string pdf_resources(const PdfContent &c, const std::string &extra);
void pdf_load_map_file(const std::string &spec);
void pdf_map_line(const std::string &line_in);
const PdfMapEntry *pdf_map_entry(const std::string &tfm);
void pdf_write_font(PdfFont &pf);
std::string pdf_write_to_unicode(const PdfFont &pf, const std::vector<std::string> &names);
/** @brief A token parameter's (\pdfpageattr ...) text. */
std::string pdf_toks(int32_t loc);
/** @brief Whether this job made PDF pages (the end of the job writes the PDF, not the DVI file). */
bool pdf_output_used();
int32_t pdf_write_outlines();
void pdf_finish_file();
/** @brief Writes what pdfTeX's primitives set up in INITEX (\\pdfglyphtounicode, \\pdfmapline ...) into the format. */
void pdf_dump_state();
/** @brief Reads it back; false if the format's data is not what mep-tex wrote. */
bool pdf_undump_state();
/** @brief \jobname's text: the job name, in quotes if it has a space (as TeX Live gives it). */
void print_quoted_job_name();
/** @brief Logs an \openout's file, as TeX Live's engines do (`\openout3 = `x.aux'.`). */
void log_openout(int32_t j);

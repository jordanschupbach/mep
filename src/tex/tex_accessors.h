// tex.web's field macros as inline members of meptex::Engine, for the
// hand-written parts of the engine (tex_etex.cpp, tex_pdf.cpp ...): the
// generated code has the macros expanded, the C++ written by hand uses
// these. #included inside the class body by tex_engine_members.h; same
// names and meanings as the WEB macros they mirror (\S numbers given).

// \S118, \S133: one-word fields
int32_t &link(int32_t p) { return mem[p].hh.rh; }
int32_t &info(int32_t p) { return mem[p].hh.lh; }
uint16_t &type(int32_t p) { return mem[p].hh.b0; }
uint16_t &subtype(int32_t p) { return mem[p].hh.b1; }
bool is_char_node(int32_t p) const { return p >= hi_mem_min; }
// \S135: boxes
int32_t &width(int32_t p) { return mem[p + 1].int_; }
int32_t &depth(int32_t p) { return mem[p + 2].int_; }
int32_t &height(int32_t p) { return mem[p + 3].int_; }
int32_t &shift_amount(int32_t p) { return mem[p + 4].int_; }
int32_t &list_ptr(int32_t p) { return mem[p + 5].hh.rh; }
uint16_t &glue_order(int32_t p) { return mem[p + 5].hh.b1; }
uint16_t &glue_sign(int32_t p) { return mem[p + 5].hh.b0; }
double &glue_set(int32_t p) { return mem[p + 6].gr; }
// \S150: glue specifications
int32_t &glue_ref_count(int32_t p) { return mem[p].hh.rh; }
int32_t &stretch(int32_t p) { return mem[p + 2].int_; }
int32_t &shrink(int32_t p) { return mem[p + 3].int_; }
uint16_t &stretch_order(int32_t p) { return mem[p].hh.b0; }
uint16_t &shrink_order(int32_t p) { return mem[p].hh.b1; }
// \S149, \S157
int32_t &glue_ptr(int32_t p) { return mem[p + 1].hh.lh; }
int32_t &leader_ptr(int32_t p) { return mem[p + 1].hh.rh; }
int32_t &penalty(int32_t p) { return mem[p + 1].int_; }
// \S200
int32_t &token_ref_count(int32_t p) { return mem[p].hh.lh; }
// \S221: eqtb
uint16_t &eq_level(int32_t p) { return eqtb[p].hh.b1; }
uint16_t &eq_type(int32_t p) { return eqtb[p].hh.b0; }
int32_t &equiv(int32_t p) { return eqtb[p].hh.rh; }
int32_t &int_par(int32_t code) { return eqtb[tc::int_base + code].int_; }
int32_t &dimen_par(int32_t code) { return eqtb[tc::dimen_base + code].int_; }
int32_t &glue_par(int32_t code) { return eqtb[tc::glue_base + code].hh.rh; }
// \S256: the hash
int32_t &text(int32_t p) { return hash[p].rh; }
// \S302: the current input state
int32_t &state_() { return cur_input.state_field; }
int32_t &index_() { return cur_input.index_field; }
int32_t &start() { return cur_input.start_field; }
int32_t &loc() { return cur_input.loc_field; }
int32_t &limit() { return cur_input.limit_field; }
int32_t &name() { return cur_input.name_field; }
// \S213: the semantic nest
int32_t &mode() { return cur_list.mode_field; }
int32_t &head() { return cur_list.head_field; }
int32_t &tail() { return cur_list.tail_field; }
// \S554: font metrics
four_quarters char_info(int32_t fnt, int32_t chr) { return font_info[char_base[fnt] + chr].qqqq; }
int32_t char_width(int32_t fnt, four_quarters ci) { return font_info[width_base[fnt] + ci.b0].int_; }
int32_t char_height(int32_t fnt, four_quarters ci) { return font_info[height_base[fnt] + ci.b1 / 16].int_; }
int32_t char_depth(int32_t fnt, four_quarters ci) { return font_info[depth_base[fnt] + ci.b1 % 16].int_; }
int32_t char_italic(int32_t fnt, four_quarters ci) { return font_info[italic_base[fnt] + ci.b2 / 4].int_; }
bool char_exists(four_quarters ci) const { return ci.b0 > 0; }
// \S162: the list heads at the top of mem (mem_top is set at run time)
int32_t page_ins_head() const { return mem_top; }
int32_t contrib_head() const { return mem_top - 1; }
int32_t page_head() const { return mem_top - 2; }
int32_t temp_head() const { return mem_top - 3; }
int32_t hold_head() const { return mem_top - 4; }
int32_t adjust_head() const { return mem_top - 5; }
int32_t garbage() const { return mem_top - 12; }
int32_t backup_head() const { return mem_top - 13; }
// \S141, mep-etex.ch: marks
int32_t &mark_ptr(int32_t p) { return mem[p + 1].hh.rh; }
int32_t &mark_class(int32_t p) { return mem[p + 1].hh.lh; }

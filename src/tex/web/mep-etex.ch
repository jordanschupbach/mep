% mep-etex.ch -- e-TeX's extensions, for mep's TeX engine.
%
% Applied after mep-tex.ch (tools/web2cpp). It gives mep-tex the
% primitives, parameters and behaviour described in the e-TeX manual
% (etex_man.tex, in TeX Live's doc/etex), always in what e-TeX calls
% extended mode: what LaTeX and expl3 require of an engine. It is written
% from that description, not from etex.ch, and is organised differently:
%
%  * the registers 256..32767 are simply more of eqtb (number_regs of each
%    kind instead of 256), so TeX's own save stack does their grouping;
%  * mark classes are kept in plain arrays, one per kind of mark;
%  * the larger pieces (expressions, \showgroups, \scantokens, the status
%    enquiries) are hand-written C++ in src/tex/tex_etex.cpp, called from
%    here like any other routine.
%
% Written for mep (MIT licence). e-TeX is the work of the NTS team and
% Peter Breitenlohner; TeX is copyright D. E. Knuth.

@x [10.141] l.2939 - a mark belongs to a class
@d mark_ptr(#)==mem[#+1].int {head of the token list for a mark}
@y
@d mark_ptr(#)==link(#+1) {head of the token list for a mark}
@d mark_class(#)==info(#+1) {the mark class (0..32767)}
@z

@x [12.184] - e-TeX marks display boxes
    begin print(", shifted "); print_scaled(shift_amount(p));
    end;
  end;
@y
    begin print(", shifted "); print_scaled(shift_amount(p));
    end;
  if (type(p)=hlist_node)and(subtype(p)=dlist) then print(", display");
  end;
@z

@x [12.192] - TeX--XeT's text-direction nodes
begin print_esc("math");
if subtype(p)=before then print("on")
else print("off");
@y
begin if subtype(p)>after then
  begin if odd(subtype(p)) then print_esc("end")
  else print_esc("begin");
  if subtype(p)>R_code then print_char("R")
  else if subtype(p)>L_code then print_char("L")
  else print_char("M");
  end
else  begin print_esc("math");
  if subtype(p)=before then print("on")
  else print("off");
  end;
@z

@x [12.196] l.3796 - marks of other classes are shown as \marks
begin print_esc("mark"); print_mark(mark_ptr(p));
@y
begin print_esc("mark");
if mark_class(p)<>0 then
  begin print_char("s"); print_int(mark_class(p));
  end;
print_mark(mark_ptr(p));
@z

@x [16.212] l.4307 - room for \middle's delimiter pointer
  @!aux_field: memory_word;
  end;
@y
  @!aux_field: memory_word;
  @!eTeX_aux_field: pointer; {the last \.{\\left} or \.{\\middle} noad}
  end;
@z

@x [16.213] l.4320 - room for \middle's delimiter pointer
@d mode_line==cur_list.ml_field {source file line number at beginning of list}
@y
@d mode_line==cur_list.ml_field {source file line number at beginning of list}
@d delim_ptr==cur_list.eTeX_aux_field {the last left or middle noad}
@z

@x [17.224] l.4598 - 32768 skip and muskip registers
@d mu_skip_base=skip_base+256 {table of 256 ``muskip'' registers}
@d local_base=mu_skip_base+256 {beginning of region 4}
@y
@d number_regs=32768 {registers of each kind: 0..32767}
@d mu_skip_base=skip_base+number_regs {table of |number_regs| ``muskip'' registers}
@d local_base=mu_skip_base+number_regs {beginning of region 4}
@z

@x [17.230] l.4691 - \everyeof, the penalty arrays, 32768 toks and box registers
@d toks_base=local_base+10 {table of 256 token list registers}
@d box_base=toks_base+256 {table of 256 box registers}
@d cur_font_loc=box_base+256 {internal font number outside math mode}
@y
@d every_eof_loc=local_base+10 {points to token list for \.{\\everyeof}}
@d inter_line_penalties_loc=local_base+11 {the \.{\\interlinepenalties} array}
@d club_penalties_loc=local_base+12 {the \.{\\clubpenalties} array}
@d widow_penalties_loc=local_base+13 {the \.{\\widowpenalties} array}
@d display_widow_penalties_loc=local_base+14
  {the \.{\\displaywidowpenalties} array}
@d toks_base=local_base+15 {table of |number_regs| token list registers}
@d box_base=toks_base+number_regs {table of |number_regs| box registers}
@d cur_font_loc=box_base+number_regs {internal font number outside math mode}
@d every_eof==equiv(every_eof_loc)
@d inter_line_penalties_ptr==equiv(inter_line_penalties_loc)
@d club_penalties_ptr==equiv(club_penalties_loc)
@d widow_penalties_ptr==equiv(widow_penalties_loc)
@d display_widow_penalties_ptr==equiv(display_widow_penalties_loc)
@z

@x [17.231] l.4752 - \everyeof
  every_cr_loc: print_esc("everycr");
@y
  every_cr_loc: print_esc("everycr");
  every_eof_loc: print_esc("everyeof");
@z

@x [17.232] l.4769 - 32768 toks and box registers; the penalty arrays
for k:=output_routine_loc to toks_base+255 do
  eqtb[k]:=eqtb[undefined_control_sequence];
box(0):=null; eq_type(box_base):=box_ref; eq_level(box_base):=level_one;
for k:=box_base+1 to box_base+255 do eqtb[k]:=eqtb[box_base];
@y
for k:=output_routine_loc to toks_base+number_regs-1 do
  eqtb[k]:=eqtb[undefined_control_sequence];
for k:=inter_line_penalties_loc to display_widow_penalties_loc do
  eqtb[k]:=eqtb[par_shape_loc];
box(0):=null; eq_type(box_base):=box_ref; eq_level(box_base):=level_one;
for k:=box_base+1 to box_base+number_regs-1 do eqtb[k]:=eqtb[box_base];
@z

@x [17.233] l.4796 - the penalty arrays
else if n<toks_base then
@y
else if (n>=inter_line_penalties_loc)and(n<=display_widow_penalties_loc) then
  begin print_cmd_chr(set_shape,n); print_char("=");
  if equiv(n)=null then print_char("0")
  else print_int(mem[equiv(n)+1].int);
  end
else if n<toks_base then
@z

@x [17.236] l.4862 - e-TeX's integer parameters
@d int_pars=55 {total number of integer parameters}
@d count_base=int_base+int_pars {256 user \.{\\count} registers}
@d del_code_base=count_base+256 {256 delimiter code mappings}
@y
@d tracing_assigns_code=55 {show assignments}
@d tracing_groups_code=56 {show save groups}
@d tracing_ifs_code=57 {show conditionals}
@d tracing_scan_tokens_code=58 {show pseudo file open and close}
@d tracing_nesting_code=59 {show incomplete groups and ifs within files}
@d pre_display_direction_code=60 {text direction preceding a display}
@d last_line_fit_code=61 {adjustment for last line of paragraph}
@d saving_vdiscards_code=62 {save items discarded from vlists}
@d saving_hyph_codes_code=63 {save hyphenation codes for languages}
@d eTeX_state_code=64 {\eTeX\ state variables}
@d int_pars=65 {total number of integer parameters}
@d count_base=int_base+int_pars {|number_regs| user \.{\\count} registers}
@d del_code_base=count_base+number_regs {256 delimiter code mappings}
@z

@x [17.236] l.4919 - e-TeX's integer parameters
@d error_context_lines==int_par(error_context_lines_code)
@y
@d error_context_lines==int_par(error_context_lines_code)
@d tracing_assigns==int_par(tracing_assigns_code)
@d tracing_groups==int_par(tracing_groups_code)
@d tracing_ifs==int_par(tracing_ifs_code)
@d tracing_scan_tokens==int_par(tracing_scan_tokens_code)
@d tracing_nesting==int_par(tracing_nesting_code)
@d pre_display_direction==int_par(pre_display_direction_code)
@d last_line_fit==int_par(last_line_fit_code)
@d saving_vdiscards==int_par(saving_vdiscards_code)
@d saving_hyph_codes==int_par(saving_hyph_codes_code)
@d TeXXeT_state==int_par(eTeX_state_code) {only one state variable so far}
@z

@x [17.237] l.4980 - e-TeX's integer parameters
error_context_lines_code:print_esc("errorcontextlines");
@y
error_context_lines_code:print_esc("errorcontextlines");
tracing_assigns_code:print_esc("tracingassigns");
tracing_groups_code:print_esc("tracinggroups");
tracing_ifs_code:print_esc("tracingifs");
tracing_scan_tokens_code:print_esc("tracingscantokens");
tracing_nesting_code:print_esc("tracingnesting");
pre_display_direction_code:print_esc("predisplaydirection");
last_line_fit_code:print_esc("lastlinefit");
saving_vdiscards_code:print_esc("savingvdiscards");
saving_hyph_codes_code:print_esc("savinghyphcodes");
eTeX_state_code:print_esc("TeXXeTstate");
@z

@x [17.247] l.5226 - 32768 dimen registers
@d eqtb_size=scaled_base+255 {largest subscript of |eqtb|}
@y
@d eqtb_size=scaled_base+number_regs-1 {largest subscript of |eqtb|}
@z

@x [18.266] l.5728 - \unless
expand_after: print_esc("expandafter");
@y
expand_after: if chr_code=0 then print_esc("expandafter")
  else print_esc("unless");
@z

@x [18.266] l.5734 - \marks
mark: print_esc("mark");
@y
mark: begin print_esc("mark"); if chr_code>0 then print_char("s");
  end;
@z

@x [18.266] - \readline
read_to_cs: print_esc("read");
@y
read_to_cs: if chr_code=0 then print_esc("read")@+else print_esc("readline");
@z

@x [18.266] l.5749 - the penalty arrays
set_shape: print_esc("parshape");
the: print_esc("the");
@y
set_shape: case chr_code of
  inter_line_penalties_loc: print_esc("interlinepenalties");
  club_penalties_loc: print_esc("clubpenalties");
  widow_penalties_loc: print_esc("widowpenalties");
  display_widow_penalties_loc: print_esc("displaywidowpenalties");
  othercases print_esc("parshape")
  endcases;
the: if chr_code=0 then print_esc("the")
  else if chr_code=1 then print_esc("unexpanded")
  else print_esc("detokenize");
@z

@x [18.266] - TeX--XeT's text-direction commands
valign: print_esc("valign");
@y
valign: if chr_code<>0 then case chr_code of
  begin_L_code: print_esc("beginL");
  end_L_code: print_esc("endL");
  begin_R_code: print_esc("beginR");
  othercases print_esc("endR")
  endcases
  else print_esc("valign");
@z

@x [19.274] l.5866 - where each group began; \tracinggroups
begin check_full_save_stack;
save_type(save_ptr):=level_boundary; save_level(save_ptr):=cur_group;
@y
begin check_full_save_stack;
saved(0):=line; incr(save_ptr); {the line this group began on}
save_type(save_ptr):=level_boundary; save_level(save_ptr):=cur_group;
@z

@x [19.274] l.5872 - where each group began; \tracinggroups
cur_boundary:=save_ptr; incr(cur_level); incr(save_ptr); cur_group:=c;
end;
@y
cur_boundary:=save_ptr; incr(cur_level); incr(save_ptr); cur_group:=c;
if tracing_groups>0 then group_trace(false);
end;
@z

@x [19.277] l.5903 - reassignments are not saved; \tracingassigns
@p procedure eq_define(@!p:pointer;@!t:quarterword;@!e:halfword);
  {new data for |eqtb|}
begin if eq_level(p)=cur_level then eq_destroy(eqtb[p])
else if cur_level>level_one then eq_save(p,eq_level(p));
eq_level(p):=cur_level; eq_type(p):=t; equiv(p):=e;
end;
@y
@p procedure eq_define(@!p:pointer;@!t:quarterword;@!e:halfword);
  {new data for |eqtb|}
label exit;
begin if (eq_type(p)=t)and(equiv(p)=e) then
  begin assign_trace(p,"reassigning");@/
  eq_destroy(eqtb[p]); return;
  end;
assign_trace(p,"changing");@/
if eq_level(p)=cur_level then eq_destroy(eqtb[p])
else if cur_level>level_one then eq_save(p,eq_level(p));
eq_level(p):=cur_level; eq_type(p):=t; equiv(p):=e;
assign_trace(p,"into");@/
exit:end;
@z

@x [19.278] l.5913 - reassignments are not saved; \tracingassigns
@p procedure eq_word_define(@!p:pointer;@!w:integer);
begin if xeq_level[p]<>cur_level then
  begin eq_save(p,xeq_level[p]); xeq_level[p]:=cur_level;
  end;
eqtb[p].int:=w;
end;
@y
@p procedure eq_word_define(@!p:pointer;@!w:integer);
label exit;
begin if eqtb[p].int=w then
  begin assign_trace(p,"reassigning");@/
  return;
  end;
assign_trace(p,"changing");@/
if xeq_level[p]<>cur_level then
  begin eq_save(p,xeq_level[p]); xeq_level[p]:=cur_level;
  end;
eqtb[p].int:=w;
assign_trace(p,"into");@/
exit:end;
@z

@x [19.279] l.5924 - \tracingassigns
begin eq_destroy(eqtb[p]);
eq_level(p):=level_one; eq_type(p):=t; equiv(p):=e;
end;
@#
procedure geq_word_define(@!p:pointer;@!w:integer); {global |eq_word_define|}
begin eqtb[p].int:=w; xeq_level[p]:=level_one;
end;
@y
begin assign_trace(p,"globally changing");@/
begin eq_destroy(eqtb[p]);
eq_level(p):=level_one; eq_type(p):=t; equiv(p):=e;
end;
assign_trace(p,"into");@/
end;
@#
procedure geq_word_define(@!p:pointer;@!w:integer); {global |eq_word_define|}
begin assign_trace(p,"globally changing");@/
begin eqtb[p].int:=w; xeq_level[p]:=level_one;
end;
assign_trace(p,"into");@/
end;
@z

@x [19.282] l.5973 - where each group began; \tracinggroups
done: cur_group:=save_level(save_ptr); cur_boundary:=save_index(save_ptr)
@y
done: if tracing_groups>0 then group_trace(true);
cur_group:=save_level(save_ptr); cur_boundary:=save_index(save_ptr);
decr(save_ptr) {the line saved by |new_save_level|}
@z

@x [20.294] l.6278 - the mark of a \protected macro does not show
end_match: print("->");
@y
end_match: if c=0 then print("->");
@z

@x [21.296] - \topmarks and friends have no single meaning to show
else if cur_cmd=top_bot_mark then
@y
else if (cur_cmd=top_bot_mark)and(cur_chr<marks_code) then
@z

@x [21.298] l.6371 - room for \protected macros
procedure print_cmd_chr(@!cmd:quarterword;@!chr_code:halfword);
begin case cmd of
@y
procedure print_cmd_chr(@!cmd:quarterword;@!chr_code:halfword);
var n:integer; {temporary index}
begin case cmd of
@z

@x [21.299] l.6390 - \tracingifs
print_cmd_chr(cur_cmd,cur_chr); print_char("}");
@y
print_cmd_chr(cur_cmd,cur_chr);
if tracing_ifs>0 then
  if (cur_cmd=if_test)or(cur_cmd=fi_or_else) then show_if_status;
print_char("}");
@z

@x [22.307] l.6526 - \everyeof
@d mark_text=14 {|token_type| code for \.{\\topmark}, etc.}
@d write_text=15 {|token_type| code for \.{\\write}}
@y
@d every_eof_text=14 {|token_type| code for \.{\\everyeof}}
@d mark_text=15 {|token_type| code for \.{\\topmark}, etc.}
@d write_text=16 {|token_type| code for \.{\\write}}
@z

@x [22.311] l.6576 - the context shown continues through \scantokens pseudo-files
    if (name>17) or (base_ptr=0) then bottom_line:=true;
@y
    if (name>19) or (base_ptr=0) then bottom_line:=true;
@z

@x [22.314] l.6640 - \everyeof
every_cr_text: print_nl("<everycr> ");
@y
every_cr_text: print_nl("<everycr> ");
every_eof_text: print_nl("<everyeof> ");
@z

@x [23.323] l.6784 - \everyeof
      othercases print_cmd_chr(assign_toks,t-output_text+output_routine_loc)
@y
      every_eof_text:print_esc("everyeof");
      othercases print_cmd_chr(assign_toks,t-output_text+output_routine_loc)
@z

@x [23.328] l.6846 - \everyeof
line_stack[index]:=line; start:=first; state:=mid_line;
@y
line_stack[index]:=line; start:=first; state:=mid_line;
eof_seen[index]:=false;
@z

@x [24.362] l.7087 - \everyeof; \scantokens pseudo-files show no parentheses
  begin if input_ln(cur_file,true) then {not end of file}
    firm_up_the_line {this sets |limit|}
  else force_eof:=true;
  end;
if force_eof then
  begin print_char(")"); decr(open_parens);
  update_terminal; {show user that file has been read}
@y
  begin if input_ln(cur_file,true) then {not end of file}
    firm_up_the_line {this sets |limit|}
  else if (every_eof<>null)and not eof_seen[index] then
    begin limit:=first-1; eof_seen[index]:=true; {fake one empty line}
    begin_token_list(every_eof,every_eof_text); goto restart;
    end
  else force_eof:=true;
  end;
if force_eof then
  begin if name>=19 then {a real file, or a traced pseudo-file}
    begin print_char(")"); decr(open_parens);
    update_terminal; {show user that file has been read}
    end;
@z

@x [25.367] l.7663 - \tracingifs
begin if tracing_commands>1 then show_cur_cmd_chr;
@y
begin if tracing_commands>1 then show_cur_cmd_chr
else if tracing_ifs>0 then
  if (cur_cmd=if_test)or(cur_cmd=fi_or_else) then show_cur_cmd_chr;
@z

@x [25.368] l.7680 - \unless
@<Expand the token after...@>=
begin get_token; t:=cur_tok; get_token;
if cur_cmd>max_command then expand@+else back_input;
cur_tok:=t; back_input;
end
@y
@<Expand the token after...@>=
if cur_chr=0 then
  begin get_token; t:=cur_tok; get_token;
  if cur_cmd>max_command then expand@+else back_input;
  cur_tok:=t; back_input;
  end
else @<Negate a boolean conditional, or complain@>
@z

@x [25.377] l.7777 - \scantokens
input: if chr_code=0 then print_esc("input")@+else print_esc("endinput");
@y
input: if chr_code=0 then print_esc("input")
  else if chr_code=2 then print_esc("scantokens")
  else print_esc("endinput");
@z

@x [25.378] l.7781 - \scantokens
if cur_chr>0 then force_eof:=true
@y
if cur_chr=1 then force_eof:=true
else if cur_chr=2 then pseudo_start
@z

@x [25.385] - \topmarks and friends
top_bot_mark: case chr_code of
  first_mark_code: print_esc("firstmark");
  bot_mark_code: print_esc("botmark");
  split_first_mark_code: print_esc("splitfirstmark");
  split_bot_mark_code: print_esc("splitbotmark");
  othercases print_esc("topmark")
  endcases;
@y
top_bot_mark: begin case (chr_code mod marks_code) of
  first_mark_code: print_esc("firstmark");
  bot_mark_code: print_esc("botmark");
  split_first_mark_code: print_esc("splitfirstmark");
  split_bot_mark_code: print_esc("splitbotmark");
  othercases print_esc("topmark")
  endcases;
  if chr_code>=marks_code then print_char("s");
  end;
@z

@x [25.386] - \topmarks and friends
begin if cur_mark[cur_chr]<>null then
  begin_token_list(cur_mark[cur_chr],mark_text);
end
@y
begin t:=cur_chr mod marks_code;
if cur_chr>=marks_code then scan_eight_bit_int@+else cur_val:=0;
if cur_val=0 then p:=cur_mark[t]@+else p:=class_mark(cur_val,t);
if p<>null then begin_token_list(p,mark_text);
end
@z

@x [25.389] l.7882 - \protected macros
warning_index:=cur_cs; ref_count:=cur_chr; r:=link(ref_count); n:=0;
@y
warning_index:=cur_cs; ref_count:=cur_chr; r:=link(ref_count); n:=0;
if info(r)=protected_token then r:=link(r);
@z

@x [26.417] l.8338 - \interactionmode
set_page_int: if chr_code=0 then print_esc("deadcycles")
@+else print_esc("insertpenalties");
@y
set_page_int: if chr_code=0 then print_esc("deadcycles")
  else if chr_code=2 then print_esc("interactionmode")
  else print_esc("insertpenalties");
@z

@x [26.417] l.8347 - e-TeX's status enquiries
  input_line_no_code: print_esc("inputlineno");
  othercases print_esc("badness")
@y
  input_line_no_code: print_esc("inputlineno");
  badness_code: print_esc("badness");
  othercases print_etex_last_item(chr_code)
@z

@x [26.419] l.8371 - \interactionmode
begin if m=0 then cur_val:=dead_cycles@+else cur_val:=insert_penalties;
@y
begin if m=0 then cur_val:=dead_cycles
else if m=2 then cur_val:=interaction
else cur_val:=insert_penalties;
@z

@x [26.423] l.8403 - the penalty arrays
begin if par_shape_ptr=null then cur_val:=0
@y
begin if m>par_shape_loc then cur_val:=fetch_penalties(m)
else if par_shape_ptr=null then cur_val:=0
@z

@x [26.424] l.8409 - e-TeX's status enquiries
if cur_chr>glue_val then
@y
if cur_chr>=eTeX_version_code then fetch_etex_item(cur_chr)
else if cur_chr>glue_val then
@z

@x [26.433] l.8481 - registers 0..32767
procedure scan_eight_bit_int;
begin scan_int;
if (cur_val<0)or(cur_val>255) then
  begin print_err("Bad register code");
@.Bad register code@>
  help2("A register number must be between 0 and 255.")@/
@y
procedure scan_eight_bit_int; {a register number, despite the name}
begin scan_int;
if (cur_val<0)or(cur_val>number_regs-1) then
  begin print_err("Bad register code");
  help2("A register number must be between 0 and 32767.")@/
@z

@x [27.465] l.9188 - \unexpanded and \detokenize
@p function the_toks:pointer;
var old_setting:0..max_selector; {holds |selector| setting}
@!p,@!q,@!r:pointer; {used for copying a token list}
@!b:pool_pointer; {base of temporary string}
begin get_x_token; scan_something_internal(tok_val,false);
@y
@p function the_toks:pointer;
label exit;
var old_setting:0..max_selector; {holds |selector| setting}
@!p,@!q,@!r:pointer; {used for copying a token list}
@!b:pool_pointer; {base of temporary string}
@!c:small_number; {the command code: \.{\\the}, \.{\\unexpanded} or \.{\\detokenize}}
begin @<Handle \.{\\unexpanded} or \.{\\detokenize} and |return|@>;
get_x_token; scan_something_internal(tok_val,false);
@z

@x [27.465] l.9204 - \unexpanded and \detokenize
  selector:=old_setting; the_toks:=str_toks(b);
  end;
end;
@y
  selector:=old_setting; the_toks:=str_toks(b);
  end;
exit:end;
@z

@x [27.468] l.9241 - \eTeXrevision
@d job_name_code=5 {command code for \.{\\jobname}}
@y
@d job_name_code=5 {command code for \.{\\jobname}}
@d eTeX_revision_code=6 {command code for \.{\\eTeXrevision}}
@d eTeX_version=2 {\.{\\eTeXversion}}
@d eTeX_revision==".6" {\.{\\eTeXrevision}}
@z

@x [27.469] l.9265 - \eTeXrevision
  font_name_code: print_esc("fontname");
@y
  font_name_code: print_esc("fontname");
  eTeX_revision_code: print_esc("eTeXrevision");
@z

@x [27.470] l.9273 - \eTeXrevision
@!c:number_code..job_name_code; {desired type of conversion}
@y
@!c:number_code..eTeX_revision_code; {desired type of conversion}
@z

@x [27.471] l.9289 - \eTeXrevision
job_name_code: if job_name=0 then open_log_file;
@y
job_name_code: if job_name=0 then open_log_file;
eTeX_revision_code: do_nothing;
@z

@x [27.472] l.9306 - \eTeXrevision
job_name_code: print(job_name);
@y
job_name_code: print(job_name);
eTeX_revision_code: print(eTeX_revision);
@z

@x [27.478] l.9396 - \protected macros are not expanded in \edef
begin loop begin get_next;
  if cur_cmd<=max_command then goto done2;
@y
begin loop begin get_next;
  if cur_cmd>=call then
    if info(link(cur_chr))=protected_token then
      begin cur_cmd:=relax; cur_chr:=no_expand_flag;
      end;
  if cur_cmd<=max_command then goto done2;
@z

@x [27.482] - \readline
@p procedure read_toks(@!n:integer;@!r:pointer);
@y
@p procedure read_toks(@!n:integer;@!r:pointer;@!j:halfword);
@z

@x [27.483] - \readline
first:=limit+1; loc:=start; state:=new_line;@/
@y
first:=limit+1; loc:=start; state:=new_line;@/
@<Handle \.{\\readline} and |goto done|@>;@/
@z

@x [28.487] l.9545 - e-TeX's conditionals
@d if_case_code=16 { `\.{\\ifcase}' }
@y
@d if_case_code=16 { `\.{\\ifcase}' }
@d if_def_code=17 { `\.{\\ifdefined}' }
@d if_cs_code=18 { `\.{\\ifcsname}' }
@d if_font_char_code=19 { `\.{\\iffontchar}' }
@d unless_code=32 {amount added for `\.{\\unless}' prefix}
@z

@x [28.488] l.9584 - e-TeX's conditionals
if_test: case chr_code of
@y
if_test: begin if chr_code>=unless_code then print_esc("unless");
case chr_code mod unless_code of
@z

@x [28.488] l.9600 - e-TeX's conditionals
  if_case_code:print_esc("ifcase");
  othercases print_esc("if")
  endcases;
@y
  if_case_code:print_esc("ifcase");
  if_def_code:print_esc("ifdefined");
  if_cs_code:print_esc("ifcsname");
  if_font_char_code:print_esc("iffontchar");
  othercases print_esc("if")
  endcases;
  end;
@z

@x [28.494] l.9673 - \tracingifs shows what ends skipped text
done: scanner_status:=save_scanner_status;
@y
done: scanner_status:=save_scanner_status;
if tracing_ifs>0 then show_cur_cmd_chr;
@z

@x [28.498] l.9744 - \unless
@!this_if:small_number; {type of this conditional}
begin @<Push the condition stack@>;@+save_cond_ptr:=cond_ptr;this_if:=cur_chr;@/
@<Either process \.{\\ifcase} or set |b| to the value of a boolean condition@>;
@y
@!this_if:small_number; {type of this conditional}
@!is_unless:boolean; {was this if preceded by `\.{\\unless}' ?}
begin @<Push the condition stack@>;@+save_cond_ptr:=cond_ptr;
is_unless:=(cur_chr>=unless_code); this_if:=cur_chr mod unless_code;@/
@<Either process \.{\\ifcase} or set |b| to the value of a boolean condition@>;
if is_unless then b:=not b;
@z

@x [28.501] l.9796 - e-TeX's conditionals
if_case_code: @<Select the appropriate case
  and |return| or |goto common_ending|@>;
@y
if_case_code: @<Select the appropriate case
  and |return| or |goto common_ending|@>;
if_def_code: begin save_scanner_status:=scanner_status;
  scanner_status:=normal; get_next; b:=(cur_cmd<>undefined_cs);
  scanner_status:=save_scanner_status;
  end;
if_cs_code: @<Test if a control sequence is defined@>;
if_font_char_code: begin scan_font_ident; n:=cur_val; scan_char_num;
  if (font_bc[n]<=cur_val)and(font_ec[n]>=cur_val) then
    b:=char_exists(char_info(n)(qi(cur_val)))
  else b:=false;
  end;
@z

@x [29.536] - e-TeX's extended mode
print_two(sys_time div 60); print_char(":"); print_two(sys_time mod 60);
end
@y
print_two(sys_time div 60); print_char(":"); print_two(sys_time mod 60);
wlog_cr; wlog('entering extended mode');
end
@z

@x [34.696] - \middle
right_noad: begin print_esc("right"); print_delimiter(delimiter(p));
@y
right_noad: begin if subtype(p)=normal then print_esc("right")
  else print_esc("middle");
  print_delimiter(delimiter(p));
@z

@x [36.727] - \middle: the next segment starts afresh
done_with_noad: r:=q; r_type:=type(r);
@y
done_with_noad: r:=q; r_type:=type(r);
if r_type=right_noad then
  begin r_type:=left_noad; cur_style:=style;
  @<Set up the values of |cur_size| and |cur_mu|, based on |cur_style|@>;
  end;
@z

@x [36.760] - \middle: the next segment starts afresh
  r_type:=t;
@y
  r_type:=t;
  if type(q)=right_noad then if subtype(q)=middle_noad then
    begin r_type:=open_noad; cur_style:=style;
    @<Set up the values of |cur_size| and |cur_mu|, based on |cur_style|@>;
    end;
@z

@x [38.815] - the penalty arrays: is the paragraph followed by a display?
procedure line_break(@!final_widow_penalty:integer);
@y
procedure line_break(@!d:boolean);
@z

@x [39.866] - TeX--XeT's nodes do not change automatic breaking
math_node: begin auto_breaking:=(subtype(cur_p)=after); kern_break;
@y
math_node: begin if subtype(cur_p)<L_code then auto_breaking:=odd(subtype(cur_p));
  kern_break;
@z

@x [39.876] - the penalty arrays
post_line_break(final_widow_penalty)
@y
post_line_break(d)
@z

@x [39.877] - the penalty arrays
procedure post_line_break(@!final_widow_penalty:integer);
@y
procedure post_line_break(@!d:boolean);
@z

@x [39.890] - the penalty arrays
if cur_line+1<>best_line then
  begin pen:=inter_line_penalty;
  if cur_line=prev_graf+1 then pen:=pen+club_penalty;
  if cur_line+2=best_line then pen:=pen+final_widow_penalty;
@y
if cur_line+1<>best_line then
  begin q:=inter_line_penalties_ptr;
  if q<>null then
    begin r:=cur_line;
    if r>mem[q+1].int then r:=mem[q+1].int;
    pen:=mem[q+r+1].int;
    end
  else pen:=inter_line_penalty;
  q:=club_penalties_ptr;
  if q<>null then
    begin r:=cur_line-prev_graf;
    if r>mem[q+1].int then r:=mem[q+1].int;
    pen:=pen+mem[q+r+1].int;
    end
  else if cur_line=prev_graf+1 then pen:=pen+club_penalty;
  if d then q:=display_widow_penalties_ptr
  else q:=widow_penalties_ptr;
  if q<>null then
    begin r:=best_line-cur_line-1;
    if r>mem[q+1].int then r:=mem[q+1].int;
    pen:=pen+mem[q+r+1].int;
    end
  else if cur_line+2=best_line then
    if d then pen:=pen+display_widow_penalty
    else pen:=pen+widow_penalty;
@z

@x [44.968] - \splitdiscards
  glue_node,kern_node,penalty_node: begin q:=p; p:=link(q); link(q):=null;
    link(prev_p):=p; flush_node_list(q);
    end;
@y
  glue_node,kern_node,penalty_node: begin q:=p; p:=link(q); link(q):=null;
    link(prev_p):=p;
    if prune_saving then
      begin if split_disc=null then split_disc:=q@+else link(split_disc_tail):=q;
      split_disc_tail:=q;
      end
    else flush_node_list(q);
    end;
@z

@x [44.977] - mark classes; \splitdiscards
  delete_token_ref(split_bot_mark); split_bot_mark:=null;
  end;
@y
  delete_token_ref(split_bot_mark); split_bot_mark:=null;
  end;
vsplit_marks_init;
@z

@x [44.977] - \splitdiscards
q:=prune_page_top(q); p:=list_ptr(v); free_node(v,box_node_size);
@y
flush_node_list(split_disc); split_disc:=null;
prune_saving:=(saving_vdiscards>0);
q:=prune_page_top(q); prune_saving:=false;
p:=list_ptr(v); free_node(v,box_node_size);
@z

@x [44.979] - mark classes
else loop@+begin if type(p)=mark_node then
    if split_first_mark=null then
@y
else loop@+begin if type(p)=mark_node then
    if mark_class(p)<>0 then vsplit_class_mark(p)
    else if split_first_mark=null then
@z

@x [45.991] - \lastnodetype
last_glue:=max_halfword; last_penalty:=0; last_kern:=0;
@y
last_glue:=max_halfword; last_penalty:=0; last_kern:=0;
last_node_type:=-1;
@z

@x [45.996] - \lastnodetype
last_penalty:=0; last_kern:=0;
if type(p)=glue_node then
@y
last_penalty:=0; last_kern:=0;
last_node_type:=type(p)+1;
if type(p)=glue_node then
@z

@x [45.999] - \pagediscards
link(contrib_head):=link(p); link(p):=null; flush_node_list(p)
@y
link(contrib_head):=link(p); link(p):=null;
if saving_vdiscards>0 then
  begin if page_disc=null then page_disc:=p@+else link(page_disc_tail):=p;
  page_disc_tail:=p;
  end
else flush_node_list(p)
@z

@x [45.1012] - mark classes
  delete_token_ref(first_mark); first_mark:=null;
  end;
@y
  delete_token_ref(first_mark); first_mark:=null;
  end;
fire_up_marks_init;
@z

@x [45.1012] - mark classes
if (top_mark<>null)and(first_mark=null) then
  begin first_mark:=top_mark; add_token_ref(top_mark);
  end;
@y
if (top_mark<>null)and(first_mark=null) then
  begin first_mark:=top_mark; add_token_ref(top_mark);
  end;
fire_up_marks_done;
@z

@x [45.1016] - mark classes
@ @<Update the values of |first_mark| and |bot_mark|@>=
begin if first_mark=null then
@y
@ @<Update the values of |first_mark| and |bot_mark|@>=
if mark_class(p)<>0 then page_class_mark(p)
else begin if first_mark=null then
@z

@x [45.1026] - \pagediscards
end_graf; unsave; output_active:=false; insert_penalties:=0;@/
@y
end_graf; unsave; output_active:=false; insert_penalties:=0;@/
flush_node_list(page_disc); page_disc:=null;
@z

@x [47.1070] - \interlinepenalties is reset with \parshape
if par_shape_ptr<>null then eq_define(par_shape_loc,shape_ref,null);
@y
if par_shape_ptr<>null then eq_define(par_shape_loc,shape_ref,null);
if inter_line_penalties_ptr<>null then
  eq_define(inter_line_penalties_loc,shape_ref,null);
@z

@x [47.1071] l.20771 - 32768 box registers
@d ship_out_flag==box_flag+512 {context code for `\.{\\shipout}'}
@d leader_flag==box_flag+513 {context code for `\.{\\leaders}'}
@y
@d global_box_flag==(box_flag+number_regs) {context code for `\.{\\global\\setbox0}'}
@d ship_out_flag==(box_flag+2*number_regs) {context code for `\.{\\shipout}'}
@d leader_flag==(ship_out_flag+1) {context code for `\.{\\leaders}'}
@z

@x [47.1077] l.20879 - 32768 box registers
if box_context<box_flag+256 then
  eq_define(box_base-box_flag+box_context,box_ref,cur_box)
else geq_define(box_base-box_flag-256+box_context,box_ref,cur_box)
@y
if box_context<global_box_flag then
  eq_define(box_base-box_flag+box_context,box_ref,cur_box)
else geq_define(box_base-global_box_flag+box_context,box_ref,cur_box)
@z

@x [47.1096] - the penalty arrays
  else line_break(widow_penalty);
@y
  else line_break(false);
@z

@x [47.1099] l.21114 - insertion classes stay 0..254
else  begin scan_eight_bit_int;
  if cur_val=255 then
@y
else  begin scan_int;
  if (cur_val<0)or(cur_val>255) then
    begin print_err("Bad register code");
    help2("A register number must be between 0 and 255.")@/
      ("I changed this one to zero."); int_error(cur_val); cur_val:=0;
    end;
  if cur_val=255 then
@z

@x [47.1101] - \marks
var p:pointer; {new node}
begin p:=scan_toks(false,true); p:=get_node(small_node_size);
@y
var p:pointer; {new node}
@!c:halfword; {the mark class}
begin if cur_chr=0 then c:=0
else  begin scan_eight_bit_int; c:=cur_val;
  end;
p:=scan_toks(false,true); p:=get_node(small_node_size);
mark_class(p):=c;
@z

@x [47.1108] - \pagediscards and \splitdiscards
un_vbox: if chr_code=copy_code then print_esc("unvcopy")
  else print_esc("unvbox");
@y
un_vbox: if chr_code=copy_code then print_esc("unvcopy")
  else if chr_code=last_box_code then print_esc("pagediscards")
  else if chr_code=vsplit_code then print_esc("splitdiscards")
  else print_esc("unvbox");
@z

@x [47.1110] - \pagediscards and \splitdiscards
label exit;
var p:pointer; {the box}
@!c:box_code..copy_code; {should we copy?}
begin c:=cur_chr; scan_eight_bit_int; p:=box(cur_val);
@y
label done,exit;
var p:pointer; {the box}
@!c:box_code..copy_code; {should we copy?}
begin if cur_chr>copy_code then
  begin if cur_chr=last_box_code then
    begin link(tail):=page_disc; page_disc:=null;
    end
  else  begin link(tail):=split_disc; split_disc:=null;
    end;
  goto done;
  end;
c:=cur_chr; scan_eight_bit_int; p:=box(cur_val);
@z

@x [47.1110] - \pagediscards and \splitdiscards
while link(tail)<>null do tail:=link(tail);
exit:end;
@y
done: while link(tail)<>null do tail:=link(tail);
exit:end;
@z

@x [47.1130] - TeX--XeT's text-direction commands
vmode+halign,hmode+valign:init_align;
@y
vmode+halign:init_align;
hmode+valign:if cur_chr>0 then
    begin if TeXXeT_state>0 then tail_append(new_math(0,cur_chr))
    else  begin print_err("Improper "); print_cmd_chr(cur_cmd,cur_chr);
      help1("Sorry, this optional e-TeX feature has been disabled."); error;
      end;
    end
  else init_align;
@z

@x [48.1136] - \middle
begin push_nest; mode:=-mmode; incompleat_noad:=null; new_save_level(c);
@y
begin push_nest; mode:=-mmode; incompleat_noad:=null; delim_ptr:=null;
new_save_level(c);
@z

@x [48.1145] - the penalty arrays
else  begin line_break(display_widow_penalty);@/
@y
else  begin line_break(true);@/
@z

@x [48.1185] - \middle
  if type(q)<>left_noad then confusion("right");
@:this can't happen right}{\quad right@>
  info(numerator(incompleat_noad)):=link(q);
  link(q):=incompleat_noad; link(incompleat_noad):=p;
@y
  if (type(q)<>left_noad)or(delim_ptr=null) then confusion("right");
  info(numerator(incompleat_noad)):=link(delim_ptr);
  link(delim_ptr):=incompleat_noad; link(incompleat_noad):=p;
@z

@x [48.1189] - \middle
left_right: if chr_code=left_noad then print_esc("left")
else print_esc("right");
@y
left_right: if chr_code=left_noad then print_esc("left")
else if chr_code=middle_noad then print_esc("middle")
else print_esc("right");
@z

@x [48.1191] - \middle
var t:small_number; {|left_noad| or |right_noad|}
@!p:pointer; {new noad}
begin t:=cur_chr;
if (t=right_noad)and(cur_group<>math_left_group) then
  @<Try to recover from mismatched \.{\\right}@>
else  begin p:=new_noad; type(p):=t;
  scan_delimiter(delimiter(p),false);
  if t=left_noad then
    begin push_math(math_left_group); link(head):=p; tail:=p;
    end
  else  begin p:=fin_mlist(p); unsave; {end of |math_left_group|}
    tail_append(new_noad); type(tail):=inner_noad;
    math_type(nucleus(tail)):=sub_mlist;
    info(nucleus(tail)):=p;
    end;
  end;
end;
@y
var t:small_number; {|left_noad|, |middle_noad| or |right_noad|}
@!p:pointer; {new noad}
@!q:pointer; {the list so far}
begin t:=cur_chr;
if (t<>left_noad)and(cur_group<>math_left_group) then
  @<Try to recover from mismatched \.{\\right}@>
else  begin p:=new_noad; type(p):=t;
  scan_delimiter(delimiter(p),false);
  if t=middle_noad then
    begin type(p):=right_noad; subtype(p):=middle_noad;
    end;
  if t=left_noad then q:=p
  else  begin q:=fin_mlist(p); unsave; {end of |math_left_group|}
    end;
  if t<>right_noad then
    begin push_math(math_left_group); link(head):=q; tail:=p;
    delim_ptr:=p;
    end
  else  begin
    tail_append(new_noad); type(tail):=inner_noad;
    math_type(nucleus(tail)):=sub_mlist;
    info(nucleus(tail)):=q;
    end;
  end;
end;
@z

@x [48.1192] - \middle
  print_err("Extra "); print_esc("right");
@.Extra \\right.@>
  help1("I'm ignoring a \right that had no matching \left.");
@y
  print_err("Extra ");
  if t=middle_noad then
    begin print_esc("middle");
    help1("I'm ignoring a \middle that had no matching \left.");
    end
  else  begin print_esc("right");
    help1("I'm ignoring a \right that had no matching \left.");
    end;
@z

@x [48.1203] - e-TeX marks display boxes
  begin shift_amount(a):=s; append_to_vlist(a);
@y
  begin shift_amount(a):=s; subtype(a):=dlist; append_to_vlist(a);
@z

@x [48.1204] - e-TeX marks display boxes
shift_amount(b):=s+d; append_to_vlist(b)
@y
shift_amount(b):=s+d; subtype(b):=dlist; append_to_vlist(b)
@z

@x [48.1205] - e-TeX marks display boxes
  shift_amount(a):=s+z-width(a);
  append_to_vlist(a);
@y
  shift_amount(a):=s+z-width(a); subtype(a):=dlist;
  append_to_vlist(a);
@z

@x [49.1209] l.22812 - \protected
prefix: if chr_code=1 then print_esc("long")
  else if chr_code=2 then print_esc("outer")
  else print_esc("global");
@y
prefix: if chr_code=1 then print_esc("long")
  else if chr_code=2 then print_esc("outer")
  else if chr_code=8 then print_esc("protected")
  else print_esc("global");
@z

@x [49.1211] l.22738 - \tracingcommands>2 shows the command after the prefixes
  if cur_cmd<=max_non_prefixed_command then
    @<Discard erroneous prefixes and |return|@>;
  end;
@y
  if cur_cmd<=max_non_prefixed_command then
    @<Discard erroneous prefixes and |return|@>;
  if tracing_commands>2 then show_cur_cmd_chr;
  end;
@z

@x [49.1213] l.22880 - \protected
if (cur_cmd<>def)and(a mod 4<>0) then
  begin print_err("You can't use `"); print_esc("long"); print("' or `");
  print_esc("outer"); print("' with `");
@.You can't use \\long...@>
  print_cmd_chr(cur_cmd,cur_chr); print_char("'");
  help1("I'll pretend you didn't say \long or \outer here.");
@y
if a>=8 then
  begin j:=protected_token; a:=a-8;
  end
else j:=0;
if (cur_cmd<>def)and((a mod 4<>0)or(j<>0)) then
  begin print_err("You can't use `"); print_esc("long"); print("' or `");
  print_esc("outer"); print("' or `"); print_esc("protected");
  print("' with `");
  print_cmd_chr(cur_cmd,cur_chr); print_char("'");
  help1("I'll pretend you didn't say \long or \outer or \protected here.");
@z

@x [49.1218] l.22936 - \protected
  q:=scan_toks(true,e); define(p,call+(a mod 4),def_ref);
@y
  q:=scan_toks(true,e);
  if j<>0 then
    begin q:=get_avail; info(q):=j; link(q):=link(def_ref);
    link(def_ref):=q;
    end;
  define(p,call+(a mod 4),def_ref);
@z

@x [49.1225] - \readline
read_to_cs: begin scan_int; n:=cur_val;
@y
read_to_cs: begin j:=cur_chr; scan_int; n:=cur_val;
@z

@x [49.1225] - \readline
  p:=cur_cs; read_toks(n,p); define(p,call,cur_val);
@y
  p:=cur_cs; read_toks(n,p,j); define(p,call,cur_val);
@z

@x [49.1241] l.23171 - 32768 box registers
  if global then n:=256+cur_val@+else n:=cur_val;
@y
  if global then n:=number_regs+cur_val@+else n:=cur_val;
@z

@x [49.1246] l.23241 - \interactionmode
procedure alter_integer;
var c:0..1; {0 for \.{\\deadcycles}, 1 for \.{\\insertpenalties}}
begin c:=cur_chr; scan_optional_equals; scan_int;
if c=0 then dead_cycles:=cur_val
@y
procedure alter_integer;
var c:small_number;
  {0 for \.{\\deadcycles}, 1 for \.{\\insertpenalties}, 2 for \.{\\interactionmode}}
begin c:=cur_chr; scan_optional_equals; scan_int;
if c=0 then dead_cycles:=cur_val
else if c=2 then
  begin if (cur_val<batch_mode)or(cur_val>error_stop_mode) then
    begin print_err("Bad interaction mode (");
    print_int(cur_val); print_char(")");
    help2("Modes are 0=batch, 1=nonstop, 2=scroll, and")@/
    ("3=errorstop. Proceed, and I'll ignore this case.");
    error;
    end
  else  begin cur_chr:=cur_val; new_interaction;
    end;
  end
@z

@x [49.1248] l.23260 - the penalty arrays
set_shape: begin scan_optional_equals; scan_int; n:=cur_val;
  if n<=0 then p:=null
@y
set_shape: begin q:=cur_chr; scan_optional_equals; scan_int; n:=cur_val;
  if q>par_shape_loc then @<Assign a penalties array and |goto done|@>;
  if n<=0 then p:=null
@z

@x [49.1292] l.23580 - \showgroups, \showtokens, \showifs
  show_lists_code:print_esc("showlists");
@y
  show_lists_code:print_esc("showlists");
  show_groups:print_esc("showgroups");
  show_tokens:print_esc("showtokens");
  show_ifs:print_esc("showifs");
@z

@x [49.1293] l.23590 - \showgroups, \showtokens, \showifs
show_lists_code: begin begin_diagnostic; show_activities;
  end;
@y
show_lists_code: begin begin_diagnostic; show_activities;
  end;
show_groups: begin begin_diagnostic; show_save_groups;
  end;
show_ifs: begin begin_diagnostic; print_nl(""); print_ln; show_if_list;
  end;
show_tokens: begin scan_general_text;
  if interaction=error_stop_mode then wake_up_terminal;
  print_nl("> "); token_show(temp_head);
  flush_list(link(temp_head)); goto common_ending;
  end;
@z

@x [49.1295] l.23633 - \protected macros
call: print("macro");
long_call: print_esc("long macro");
outer_call: print_esc("outer macro");
long_outer_call: begin print_esc("long"); print_esc("outer macro");
  end;
@y
call,long_call,outer_call,long_outer_call: begin n:=cmd-call;
  if info(link(chr_code))=protected_token then n:=n+4;
  if odd(n div 4) then print_esc("protected");
  if odd(n) then print_esc("long");
  if odd(n div 2) then print_esc("outer");
  if n>0 then print_char(" ");
  print("macro");
  end;
@z

@x [51.1335] - mark classes
  begin @!init for c:=top_mark_code to split_bot_mark_code do
    if cur_mark[c]<>null then delete_token_ref(cur_mark[c]);
@y
  begin @!init for c:=top_mark_code to split_bot_mark_code do
    if cur_mark[c]<>null then delete_token_ref(cur_mark[c]);
  class_marks_cleanup;
  flush_node_list(page_disc); page_disc:=null;
  flush_node_list(split_disc); split_disc:=null;
@z

@x [51.1337] - e-TeX's extended mode
@<Initialize the print |selector|...@>;
@y
@<Initialize the print |selector|...@>;
wterm_ln('entering extended mode');
@z

@x [54.1379] l.24993 - e-TeX's new sections
itself will get a new section number.
@y
itself will get a new section number.

@* \[54/e-TeX] The e-TeX extensions.
These sections hold what the changes above need that has no home in
\TeX's own sections: e-TeX's new command codes and primitives, and a few
procedures. The larger algorithms are in \.{tex\_etex.cpp}.

@d protected_token=@'7001 {$2^8\cdot|end_match|+1$: marks a \.{\\protected} macro}
@d eTeX_version_code=badness_code+1 {code for \.{\\eTeXversion}}
@d current_group_level_code=eTeX_version_code+1 {code for \.{\\currentgrouplevel}}
@d current_group_type_code=eTeX_version_code+2 {code for \.{\\currentgrouptype}}
@d current_if_level_code=eTeX_version_code+3 {code for \.{\\currentiflevel}}
@d current_if_type_code=eTeX_version_code+4 {code for \.{\\currentiftype}}
@d current_if_branch_code=eTeX_version_code+5 {code for \.{\\currentifbranch}}
@d glue_stretch_order_code=eTeX_version_code+6 {code for \.{\\gluestretchorder}}
@d glue_shrink_order_code=eTeX_version_code+7 {code for \.{\\glueshrinkorder}}
@d last_node_type_code=eTeX_version_code+8 {code for \.{\\lastnodetype}}
@d font_char_wd_code=eTeX_version_code+9 {code for \.{\\fontcharwd}}
@d font_char_ht_code=eTeX_version_code+10 {code for \.{\\fontcharht}}
@d font_char_dp_code=eTeX_version_code+11 {code for \.{\\fontchardp}}
@d font_char_ic_code=eTeX_version_code+12 {code for \.{\\fontcharic}}
@d par_shape_length_code=eTeX_version_code+13 {code for \.{\\parshapelength}}
@d par_shape_indent_code=eTeX_version_code+14 {code for \.{\\parshapeindent}}
@d par_shape_dimen_code=eTeX_version_code+15 {code for \.{\\parshapedimen}}
@d glue_stretch_code=eTeX_version_code+16 {code for \.{\\gluestretch}}
@d glue_shrink_code=eTeX_version_code+17 {code for \.{\\glueshrink}}
@d mu_to_glue_code=eTeX_version_code+18 {code for \.{\\mutoglue}}
@d glue_to_mu_code=eTeX_version_code+19 {code for \.{\\gluetomu}}
@d eTeX_expr=eTeX_version_code+20 {first of the expression codes}
@d num_expr_code=eTeX_expr {code for \.{\\numexpr}}
@d dimen_expr_code=eTeX_expr+1 {code for \.{\\dimexpr}}
@d glue_expr_code=eTeX_expr+2 {code for \.{\\glueexpr}}
@d mu_expr_code=eTeX_expr+3 {code for \.{\\muexpr}}
@d show_groups=4 { \.{\\showgroups} }
@d show_tokens=5 { \.{\\showtokens} }
@d show_ifs=6 { \.{\\showifs} }
@d dlist=2 {|subtype| of the |hlist_node| of a display}
@d marks_code=5 {|top_bot_mark| codes for \.{\\topmarks} etc. add this}
@d middle_noad=1 {|subtype| of a right noad that is a \.{\\middle}}
@d begin_M_code=2 {math node |subtype| beginning math in TeX--XeT}
@d end_M_code=3 {math node |subtype| ending math in TeX--XeT}
@d L_code=4 {\.{\\beginL} and \.{\\endL} add this}
@d begin_L_code=L_code+begin_M_code {\.{\\beginL}}
@d end_L_code=L_code+end_M_code {\.{\\endL}}
@d R_code=L_code+L_code {\.{\\beginR} and \.{\\endR} add this}
@d begin_R_code=R_code+begin_M_code {\.{\\beginR}}
@d end_R_code=R_code+end_M_code {\.{\\endR}}
@d pseudo_name=18 {|name| of a \.{\\scantokens} pseudo-file}
@d traced_pseudo_name=19 {|name| of one traced by \.{\\tracingscantokens}}

@<Glob...@>=
@!eof_seen:array[1..max_in_open] of boolean; {has \.{\\everyeof} been read?}
@!last_node_type:integer; {used to implement \.{\\lastnodetype}}
@!page_disc,@!page_disc_tail:pointer; {items discarded by the page builder}
@!split_disc,@!split_disc_tail:pointer; {items discarded by \.{\\vsplit}}
@!prune_saving:boolean; {should |prune_page_top| keep what it discards?}

@ @<Set init...@>=
last_node_type:=-1;
page_disc:=null; page_disc_tail:=null; split_disc:=null; split_disc_tail:=null;
prune_saving:=false;

@ @<Put each...@>=
primitive("protected",prefix,8);@/
primitive("unexpanded",the,1);@/
primitive("detokenize",the,show_tokens);@/
primitive("showgroups",xray,show_groups);@/
primitive("showtokens",xray,show_tokens);@/
primitive("showifs",xray,show_ifs);@/
primitive("unless",expand_after,1);@/
primitive("ifdefined",if_test,if_def_code);@/
primitive("ifcsname",if_test,if_cs_code);@/
primitive("iffontchar",if_test,if_font_char_code);@/
primitive("eTeXversion",last_item,eTeX_version_code);@/
primitive("eTeXrevision",convert,eTeX_revision_code);@/
primitive("currentgrouplevel",last_item,current_group_level_code);@/
primitive("currentgrouptype",last_item,current_group_type_code);@/
primitive("currentiflevel",last_item,current_if_level_code);@/
primitive("currentiftype",last_item,current_if_type_code);@/
primitive("currentifbranch",last_item,current_if_branch_code);@/
primitive("gluestretchorder",last_item,glue_stretch_order_code);@/
primitive("glueshrinkorder",last_item,glue_shrink_order_code);@/
primitive("lastnodetype",last_item,last_node_type_code);@/
primitive("fontcharwd",last_item,font_char_wd_code);@/
primitive("fontcharht",last_item,font_char_ht_code);@/
primitive("fontchardp",last_item,font_char_dp_code);@/
primitive("fontcharic",last_item,font_char_ic_code);@/
primitive("parshapelength",last_item,par_shape_length_code);@/
primitive("parshapeindent",last_item,par_shape_indent_code);@/
primitive("parshapedimen",last_item,par_shape_dimen_code);@/
primitive("gluestretch",last_item,glue_stretch_code);@/
primitive("glueshrink",last_item,glue_shrink_code);@/
primitive("mutoglue",last_item,mu_to_glue_code);@/
primitive("gluetomu",last_item,glue_to_mu_code);@/
primitive("numexpr",last_item,num_expr_code);@/
primitive("dimexpr",last_item,dimen_expr_code);@/
primitive("glueexpr",last_item,glue_expr_code);@/
primitive("muexpr",last_item,mu_expr_code);@/
primitive("interactionmode",set_page_int,2);@/
primitive("scantokens",input,2);@/
primitive("everyeof",assign_toks,every_eof_loc);@/
primitive("interlinepenalties",set_shape,inter_line_penalties_loc);@/
primitive("clubpenalties",set_shape,club_penalties_loc);@/
primitive("widowpenalties",set_shape,widow_penalties_loc);@/
primitive("displaywidowpenalties",set_shape,display_widow_penalties_loc);@/
primitive("tracingassigns",assign_int,int_base+tracing_assigns_code);@/
primitive("tracinggroups",assign_int,int_base+tracing_groups_code);@/
primitive("tracingifs",assign_int,int_base+tracing_ifs_code);@/
primitive("tracingscantokens",assign_int,int_base+tracing_scan_tokens_code);@/
primitive("tracingnesting",assign_int,int_base+tracing_nesting_code);@/
primitive("predisplaydirection",assign_int,int_base+pre_display_direction_code);@/
primitive("lastlinefit",assign_int,int_base+last_line_fit_code);@/
primitive("savingvdiscards",assign_int,int_base+saving_vdiscards_code);@/
primitive("savinghyphcodes",assign_int,int_base+saving_hyph_codes_code);@/
primitive("TeXXeTstate",assign_int,int_base+eTeX_state_code);@/
primitive("marks",mark,marks_code);@/
primitive("topmarks",top_bot_mark,top_mark_code+marks_code);@/
primitive("firstmarks",top_bot_mark,first_mark_code+marks_code);@/
primitive("botmarks",top_bot_mark,bot_mark_code+marks_code);@/
primitive("splitfirstmarks",top_bot_mark,split_first_mark_code+marks_code);@/
primitive("splitbotmarks",top_bot_mark,split_bot_mark_code+marks_code);@/
primitive("middle",left_right,middle_noad);@/
primitive("pagediscards",un_vbox,last_box_code);@/
primitive("splitdiscards",un_vbox,vsplit_code);@/
primitive("readline",read_to_cs,1);@/
primitive("beginL",valign,begin_L_code);@/
primitive("endL",valign,end_L_code);@/
primitive("beginR",valign,begin_R_code);@/
primitive("endR",valign,end_R_code);@/

@ @<Negate a boolean conditional, or complain@>=
begin get_token;
if (cur_cmd=if_test)and(cur_chr<>if_case_code) then
  begin cur_chr:=cur_chr+unless_code;
  if tracing_commands>1 then show_cur_cmd_chr
  else if tracing_ifs>0 then show_cur_cmd_chr;
  conditional;
  end
else  begin print_err("You can't use `"); print_esc("unless");
  print("' before `"); print_cmd_chr(cur_cmd,cur_chr); print_char("'");
  help1("Continue, and I'll forget that it ever happened.");
  back_error;
  end;
end

@ |the_toks| is also called by \.{\\showthe}, with |cur_chr=show_the_code|
(even): the odd codes are \.{\\unexpanded} (1) and \.{\\detokenize}
(|show_tokens|).

@<Handle \.{\\unexpanded} or \.{\\detokenize} and |return|@>=
if odd(cur_chr) then
  begin c:=cur_chr; scan_general_text;
  if c=1 then the_toks:=cur_val
  else  begin old_setting:=selector; selector:=new_string; b:=pool_ptr;
    p:=get_avail; link(p):=link(temp_head);
    token_show(p); flush_list(p);
    selector:=old_setting; the_toks:=str_toks(b);
    end;
  return;
  end

@ A balanced text, unexpanded, as \.{\\unexpanded}, \.{\\detokenize},
\.{\\scantokens} and \.{\\showtokens} take it: the list is left in
|link(temp_head)|, and |cur_val| is its last node (|temp_head| when it
is empty).

@<Declare \eTeX\ procedures for scanning@>=
procedure scan_general_text;
label found;
var s:normal..absorbing; {to save |scanner_status|}
@!w:pointer; {to save |warning_index|}
@!d:pointer; {to save |def_ref|}
@!p:pointer; {tail of the token list being built}
@!q:pointer; {new node being added to the token list via |store_new_token|}
@!unbalance:halfword; {number of unmatched left braces}
begin s:=scanner_status; w:=warning_index; d:=def_ref;
scanner_status:=absorbing; warning_index:=cur_cs;
def_ref:=get_avail; token_ref_count(def_ref):=null; p:=def_ref;
scan_left_brace; unbalance:=1;
loop@+begin get_token;
  if cur_tok<right_brace_limit then
    if cur_cmd<right_brace then incr(unbalance)
    else  begin decr(unbalance);
      if unbalance=0 then goto found;
      end;
  store_new_token(cur_tok);
  end;
found: q:=link(def_ref); free_avail(def_ref); {discard reference count}
if q=null then cur_val:=temp_head@+else cur_val:=p;
link(temp_head):=q;
scanner_status:=s; warning_index:=w; def_ref:=d;
end;

@ @<Handle \.{\\readline} and |goto done|@>=
if j=1 then
  begin while loc<=limit do {current line not yet finished}
    begin cur_chr:=buffer[loc]; incr(loc);
    if cur_chr=" " then cur_tok:=space_token
    @+else cur_tok:=cur_chr+other_token;
    store_new_token(cur_tok);
    end;
  goto done;
  end

@ \.{\\tracingassigns}: each assignment subject to grouping, shown before
and after.

@<Declare \eTeX\ procedures for scanning@>=
procedure assign_trace(@!p:pointer;@!s:str_number);
begin if tracing_assigns>0 then restore_trace(p,s);
end;

@ @<Test if a control sequence is defined@>=
begin n:=get_avail; p:=n; {head of the list of characters}
repeat get_x_token;
if cur_cs=0 then store_new_token(cur_tok);
until cur_cs<>0;
if cur_cmd<>end_cs_name then @<Complain about missing \.{\\endcsname}@>;
m:=first; p:=link(n);
while p<>null do
  begin if m>=max_buf_stack then
    begin max_buf_stack:=m+1;
    if max_buf_stack=buf_size then overflow("buffer size",buf_size);
    end;
  buffer[m]:=info(p) mod @'400; incr(m); p:=link(p);
  end;
if m>first+1 then cur_cs:=id_lookup(first,m-first)
  {|no_new_control_sequence| is |true|}
else if m=first then cur_cs:=null_cs {the list is empty}
else cur_cs:=single_base+buffer[first]; {the list has length one}
flush_list(n);
b:=(eq_type(cur_cs)<>undefined_cs);
end

@ The penalty arrays are kept like \.{\\parshape}'s specification (so that
|eq_destroy| can free them the same way): |info(p)| and |mem[p+1].int|
hold the number of values $n$, and the values follow in |mem[p+2..p+n+1]|.

@<Assign a penalties array and |goto done|@>=
begin if n<=0 then p:=null
else  begin p:=get_node(2*n+1); info(p):=n; mem[p+1].int:=n;
  for j:=1 to n do
    begin scan_int; mem[p+j+1].int:=cur_val; {penalty values}
    end;
  end;
define(q,shape_ref,p); goto done;
end

@ @<Declare act...@>=
@<Declare \eTeX\ procedures for scanning@>@;
@z

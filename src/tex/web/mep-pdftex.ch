% mep-pdftex.ch -- pdfTeX's primitives, for mep's TeX engine.
%
% Applied after mep-tex.ch and mep-etex.ch (tools/web2cpp). It gives
% mep-tex the primitives of pdfTeX 1.40 as the pdfTeX manual (pdftex.tex,
% in TeX Live's doc/pdftex) describes them: the PDF output parameters, the
% expandable string and file primitives (\pdfstrcmp, \expanded, \pdffilesize
% ...), the PDF objects, annotations, links, images and forms, and the rest.
% Written from that manual, not from pdftex.web (which is GPL); the routines
% that do the work are hand-written C++ in src/tex/tex_pdf.cpp, reached from
% here like any other routine.
%
% Written for mep (MIT licence). pdfTeX is the work of Han The Thanh and
% its maintainers; TeX is copyright D. E. Knuth.

@x [10.155] - pdfTeX's margin kerns
@d explicit=1 {|subtype| of kern nodes from \.{\\kern} and \.{\\/}}
@y
@d explicit=1 {|subtype| of kern nodes from \.{\\kern} and \.{\\/}}
@d margin_kern_node=40 {|type| of the kern character protrusion puts at a margin}
@d left_side=0 {|subtype| of a margin kern at the start of a line}
@d right_side=1 {|subtype| of a margin kern at the end of a line}
@z

@x [12.174] - expanded fonts
  print_char(" "); print_ASCII(qo(character(p)));
@y
  print_font_expansion(font(p));
  print_char(" "); print_ASCII(qo(character(p)));
@z

@x [12.183] - pdfTeX's margin kerns
  kern_node: @<Display kern |p|@>;
@y
  kern_node: @<Display kern |p|@>;
  margin_kern_node: begin print_esc("kern"); print_scaled(width(p));
    if subtype(p)=left_side then print(" (left margin)")
    else print(" (right margin)");
    end;
@z

@x [14.202] - pdfTeX's margin kerns
    kern_node,math_node,penalty_node: do_nothing;
@y
    kern_node,math_node,penalty_node,margin_kern_node: do_nothing;
@z

@x [14.206] - pdfTeX's margin kerns
kern_node,math_node,penalty_node: begin r:=get_node(small_node_size);
@y
kern_node,math_node,penalty_node,margin_kern_node: begin r:=get_node(small_node_size);
@z

@x [17.230] - pdfTeX's token parameters
@d toks_base=local_base+15 {table of |number_regs| token list registers}
@y
@d pdf_pages_attr_loc=local_base+15 {\.{\\pdfpagesattr}}
@d pdf_page_attr_loc=local_base+16 {\.{\\pdfpageattr}}
@d pdf_page_resources_loc=local_base+17 {\.{\\pdfpageresources}}
@d pdf_pk_mode_loc=local_base+18 {\.{\\pdfpkmode}}
@d toks_base=local_base+19 {table of |number_regs| token list registers}
@z

@x [17.231] - pdfTeX's token parameters
  every_eof_loc: print_esc("everyeof");
@y
  every_eof_loc: print_esc("everyeof");
  pdf_pages_attr_loc: print_esc("pdfpagesattr");
  pdf_page_attr_loc: print_esc("pdfpageattr");
  pdf_page_resources_loc: print_esc("pdfpageresources");
  pdf_pk_mode_loc: print_esc("pdfpkmode");
@z

@x [17.236] - pdfTeX's integer parameters
@d int_pars=65 {total number of integer parameters}
@y
@d pdf_output_code=65 {PDF rather than DVI output}
@d pdf_compress_level_code=66
@d pdf_decimal_digits_code=67
@d pdf_move_chars_code=68
@d pdf_image_resolution_code=69
@d pdf_pk_resolution_code=70
@d pdf_unique_resname_code=71
@d pdf_force_pagebox_code=72
@d pdf_inclusion_errorlevel_code=73
@d pdf_major_version_code=74
@d pdf_minor_version_code=75
@d pdf_page_box_code=76
@d pdf_gamma_code=77
@d pdf_image_gamma_code=78
@d pdf_image_hicolor_code=79
@d pdf_image_apply_gamma_code=80
@d pdf_adjust_spacing_code=81
@d pdf_protrude_chars_code=82
@d pdf_tracing_fonts_code=83
@d pdf_obj_compress_level_code=84
@d pdf_adjust_interword_glue_code=85
@d pdf_prepend_kern_code=86
@d pdf_append_kern_code=87
@d pdf_gen_tounicode_code=88
@d pdf_draftmode_code=89
@d pdf_inclusion_copy_font_code=90
@d pdf_suppress_warning_dup_dest_code=91
@d pdf_suppress_warning_dup_map_code=92
@d pdf_suppress_warning_page_group_code=93
@d pdf_info_omit_date_code=94
@d pdf_suppress_ptex_info_code=95
@d pdf_omit_charset_code=96
@d pdf_omit_info_dict_code=97
@d pdf_omit_procset_code=98
@d show_stream_code=99
@d tracing_stack_levels_code=100
@d partoken_context_code=101
@d ignore_primitive_error_code=102
@d int_pars=103 {total number of integer parameters}
@z

@x [17.236] - pdfTeX's integer parameters
@d TeXXeT_state==int_par(eTeX_state_code) {only one state variable so far}
@y
@d TeXXeT_state==int_par(eTeX_state_code) {only one state variable so far}
@d pdf_output==int_par(pdf_output_code)
@d pdf_compress_level==int_par(pdf_compress_level_code)
@d pdf_decimal_digits==int_par(pdf_decimal_digits_code)
@d pdf_draftmode==int_par(pdf_draftmode_code)
@d pdf_protrude_chars==int_par(pdf_protrude_chars_code)
@d pdf_adjust_spacing==int_par(pdf_adjust_spacing_code)
@z

@x [17.237] - pdfTeX's integer parameters
eTeX_state_code:print_esc("TeXXeTstate");
@y
eTeX_state_code:print_esc("TeXXeTstate");
pdf_output_code:print_esc("pdfoutput");
pdf_compress_level_code:print_esc("pdfcompresslevel");
pdf_decimal_digits_code:print_esc("pdfdecimaldigits");
pdf_move_chars_code:print_esc("pdfmovechars");
pdf_image_resolution_code:print_esc("pdfimageresolution");
pdf_pk_resolution_code:print_esc("pdfpkresolution");
pdf_unique_resname_code:print_esc("pdfuniqueresname");
pdf_force_pagebox_code:print_esc("pdfforcepagebox");
pdf_inclusion_errorlevel_code:print_esc("pdfinclusionerrorlevel");
pdf_major_version_code:print_esc("pdfmajorversion");
pdf_minor_version_code:print_esc("pdfminorversion");
pdf_page_box_code:print_esc("pdfpagebox");
pdf_gamma_code:print_esc("pdfgamma");
pdf_image_gamma_code:print_esc("pdfimagegamma");
pdf_image_hicolor_code:print_esc("pdfimagehicolor");
pdf_image_apply_gamma_code:print_esc("pdfimageapplygamma");
pdf_adjust_spacing_code:print_esc("pdfadjustspacing");
pdf_protrude_chars_code:print_esc("pdfprotrudechars");
pdf_tracing_fonts_code:print_esc("pdftracingfonts");
pdf_obj_compress_level_code:print_esc("pdfobjcompresslevel");
pdf_adjust_interword_glue_code:print_esc("pdfadjustinterwordglue");
pdf_prepend_kern_code:print_esc("pdfprependkern");
pdf_append_kern_code:print_esc("pdfappendkern");
pdf_gen_tounicode_code:print_esc("pdfgentounicode");
pdf_draftmode_code:print_esc("pdfdraftmode");
pdf_inclusion_copy_font_code:print_esc("pdfinclusioncopyfonts");
pdf_suppress_warning_dup_dest_code:print_esc("pdfsuppresswarningdupdest");
pdf_suppress_warning_dup_map_code:print_esc("pdfsuppresswarningdupmap");
pdf_suppress_warning_page_group_code:print_esc("pdfsuppresswarningpagegroup");
pdf_info_omit_date_code:print_esc("pdfinfoomitdate");
pdf_suppress_ptex_info_code:print_esc("pdfsuppressptexinfo");
pdf_omit_charset_code:print_esc("pdfomitcharset");
pdf_omit_info_dict_code:print_esc("pdfomitinfodict");
pdf_omit_procset_code:print_esc("pdfomitprocset");
show_stream_code:print_esc("showstream");
tracing_stack_levels_code:print_esc("tracingstacklevels");
partoken_context_code:print_esc("partokencontext");
ignore_primitive_error_code:print_esc("ignoreprimitiveerror");
@z

@x [17.240] - pdfTeX's initial values
escape_char:="\"; end_line_char:=carriage_return;
@y
escape_char:="\"; end_line_char:=carriage_return;
int_par(pdf_compress_level_code):=9; int_par(pdf_decimal_digits_code):=3;
int_par(pdf_image_resolution_code):=72; int_par(pdf_major_version_code):=1;
int_par(pdf_minor_version_code):=4; int_par(pdf_gamma_code):=1000;
int_par(pdf_image_gamma_code):=2200; int_par(pdf_image_hicolor_code):=1;
int_par(show_stream_code):=-1;
@z

@x [17.247] - pdfTeX's dimension parameters
@d dimen_pars=21 {total number of dimension parameters}
@y
@d pdf_h_origin_code=21 {horizontal offset of the page origin}
@d pdf_v_origin_code=22 {vertical offset of the page origin}
@d pdf_page_width_code=23 {page width of the PDF output}
@d pdf_page_height_code=24 {page height of the PDF output}
@d pdf_link_margin_code=25
@d pdf_dest_margin_code=26
@d pdf_thread_margin_code=27
@d pdf_first_line_height_code=28
@d pdf_last_line_depth_code=29
@d pdf_each_line_height_code=30
@d pdf_each_line_depth_code=31
@d pdf_ignored_dimen_code=32
@d pdf_px_dimen_code=33
@d dimen_pars=34 {total number of dimension parameters}
@z

@x [17.247] - pdfTeX's dimension parameters
emergency_stretch_code:print_esc("emergencystretch");
@y
emergency_stretch_code:print_esc("emergencystretch");
pdf_h_origin_code:print_esc("pdfhorigin");
pdf_v_origin_code:print_esc("pdfvorigin");
pdf_page_width_code:print_esc("pdfpagewidth");
pdf_page_height_code:print_esc("pdfpageheight");
pdf_link_margin_code:print_esc("pdflinkmargin");
pdf_dest_margin_code:print_esc("pdfdestmargin");
pdf_thread_margin_code:print_esc("pdfthreadmargin");
pdf_first_line_height_code:print_esc("pdffirstlineheight");
pdf_last_line_depth_code:print_esc("pdflastlinedepth");
pdf_each_line_height_code:print_esc("pdfeachlineheight");
pdf_each_line_depth_code:print_esc("pdfeachlinedepth");
pdf_ignored_dimen_code:print_esc("pdfignoreddimen");
pdf_px_dimen_code:print_esc("pdfpxdimen");
@z

@x [17.250] - pdfTeX's initial values
for k:=dimen_base to eqtb_size do eqtb[k].sc:=0;
@y
for k:=dimen_base to eqtb_size do eqtb[k].sc:=0;
dimen_par(pdf_h_origin_code):=4736287; dimen_par(pdf_v_origin_code):=4736287;
  {one inch}
dimen_par(pdf_first_line_height_code):=ignore_depth;
dimen_par(pdf_last_line_depth_code):=ignore_depth;
dimen_par(pdf_each_line_height_code):=ignore_depth;
dimen_par(pdf_each_line_depth_code):=ignore_depth;
dimen_par(pdf_ignored_dimen_code):=ignore_depth;
dimen_par(pdf_px_dimen_code):=65782; {one big point}
@z

@x [18.264] - every primitive also gets a hidden twin for \pdfprimitive
eq_level(cur_val):=level_one; eq_type(cur_val):=c; equiv(cur_val):=o;
end;
@y
eq_level(cur_val):=level_one; eq_type(cur_val):=c; equiv(cur_val):=o;
store_primitive(cur_val);
end;
@z

@x [18.266] - \letterspacefont and \pdfcopyfont, made as \font is
def_font: print_esc("font");
@y
def_font: if chr_code=1 then print_esc("letterspacefont")
  else if chr_code=2 then print_esc("pdfcopyfont")
  else print_esc("font");
@z

@x [18.266] - \pdfprimitive
expand_after: if chr_code=0 then print_esc("expandafter")
  else print_esc("unless");
@y
expand_after: if chr_code=0 then print_esc("expandafter")
  else if chr_code=1 then print_esc("unless")
  else print_esc("pdfprimitive");
@z

@x [25.368] - \pdfprimitive
else @<Negate a boolean conditional, or complain@>
@y
else if cur_chr=1 then @<Negate a boolean conditional, or complain@>
else pdf_primitive_expand
@z

@x [25.372] - \ifincsname
begin r:=get_avail; p:=r; {head of the list of characters}
repeat get_x_token;
if cur_cs=0 then store_new_token(cur_tok);
until cur_cs<>0;
@y
begin r:=get_avail; p:=r; {head of the list of characters}
incr(incsname_state);
repeat get_x_token;
if cur_cs=0 then store_new_token(cur_tok);
until cur_cs<>0;
decr(incsname_state);
@z

@x [26.426] - pdfTeX's font character codes
if m=0 then scanned_result(hyphen_char[cur_val])(int_val)
else scanned_result(skew_char[cur_val])(int_val);
@y
if m=0 then scanned_result(hyphen_char[cur_val])(int_val)
else if m=1 then scanned_result(skew_char[cur_val])(int_val)
else  begin p:=cur_val; scan_char_num;
  scanned_result(font_char_code(m,p,cur_val))(int_val);
  end;
@z

@x [27.469] - pdfTeX's expandable primitives
  othercases print_esc("jobname")
@y
  job_name_code: print_esc("jobname");
  othercases print_pdf_convert(chr_code)
@z

@x [27.470] - pdfTeX's expandable primitives
@p procedure conv_toks;
@y
@p procedure conv_toks;
label exit;
@z

@x [27.470] - pdfTeX's expandable primitives
begin c:=cur_chr; @<Scan the argument for command |c|@>;
@y
begin c:=cur_chr;
if c>=pdf_first_convert_code then
  begin pdf_convert(c); return;
  end;
@<Scan the argument for command |c|@>;
@z

@x [27.470] - pdfTeX's expandable primitives
selector:=old_setting; link(garbage):=str_toks(b); ins_list(link(temp_head));
end;
@y
selector:=old_setting; link(garbage):=str_toks(b); ins_list(link(temp_head));
exit:end;
@z

@x [27.472] - a \jobname with a space comes quoted, so `\jobname.aux' is one file name
job_name_code: print(job_name);
@y
job_name_code: print_quoted_job_name;
@z

@x [28.487] - pdfTeX's conditionals
@d unless_code=32 {amount added for `\.{\\unless}' prefix}
@y
@d if_in_csname_code=20 { `\.{\\ifincsname}' }
@d if_pdfprimitive_code=21 { `\.{\\ifpdfprimitive}' }
@d if_abs_num_code=22 { `\.{\\ifpdfabsnum}' }
@d if_abs_dim_code=23 { `\.{\\ifpdfabsdim}' }
@d unless_code=32 {amount added for `\.{\\unless}' prefix}
@z

@x [28.488] - pdfTeX's conditionals
  if_font_char_code:print_esc("iffontchar");
@y
  if_font_char_code:print_esc("iffontchar");
  if_in_csname_code:print_esc("ifincsname");
  if_pdfprimitive_code:print_esc("ifpdfprimitive");
  if_abs_num_code:print_esc("ifpdfabsnum");
  if_abs_dim_code:print_esc("ifpdfabsdim");
@z

@x [28.501] - pdfTeX's conditionals
if_int_code, if_dim_code: @<Test relation between integers or dimensions@>;
@y
if_int_code, if_dim_code, if_abs_num_code, if_abs_dim_code:
  @<Test relation between integers or dimensions@>;
if_in_csname_code: b:=(incsname_state>0);
if_pdfprimitive_code: b:=pdf_if_primitive;
@z

@x [28.503] - \ifpdfabsnum and \ifpdfabsdim
begin if this_if=if_int_code then scan_int@+else scan_normal_dimen;
n:=cur_val; @<Get the next non-blank non-call...@>;
@y
begin if (this_if=if_int_code)or(this_if=if_abs_num_code) then scan_int
else scan_normal_dimen;
n:=cur_val;
if (this_if=if_abs_num_code)or(this_if=if_abs_dim_code) then n:=abs(n);
@<Get the next non-blank non-call...@>;
@z

@x [28.503] - \ifpdfabsnum and \ifpdfabsdim
if this_if=if_int_code then scan_int@+else scan_normal_dimen;
case r of
@y
if (this_if=if_int_code)or(this_if=if_abs_num_code) then scan_int
else scan_normal_dimen;
if (this_if=if_abs_num_code)or(this_if=if_abs_dim_code) then cur_val:=abs(cur_val);
case r of
@z

@x [29.526] - a file name may be given in braces
@<Get the next non-blank non-call...@>;
loop@+begin if (cur_cmd>other_char)or(cur_chr>255) then {not a character}
    begin back_input; goto done;
    end;
@y
@<Get the next non-blank non-call...@>;
if cur_cmd=left_brace then
  begin scan_braced_file_name; goto done;
  end;
loop@+begin if (cur_cmd>other_char)or(cur_chr>255) then {not a character}
    begin back_input; goto done;
    end;
@z

@x [32.622] - pdfTeX's margin kerns
kern_node,math_node:cur_h:=cur_h+width(p);
@y
kern_node,math_node,margin_kern_node:cur_h:=cur_h+width(p);
@z

@x [32.638] - PDF output
@<Ship box |p| out@>;
@y
if pdf_output>0 then pdf_ship_out(p)
else begin @<Ship box |p| out@>; end;
@z

@x [33.651] - pdfTeX's margin kerns
  kern_node,math_node: x:=x+width(p);
@y
  kern_node,math_node,margin_kern_node: x:=x+width(p);
@z

@x [38.833] - font expansion: the stretch and shrink of fonts in the widths
@d delta_node_size=7 {number of words in a delta node}
@y
@d delta_node_size=9 {number of words in a delta node}
@z

@x [38.833] - font expansion: the stretch and shrink of fonts in the widths
@d do_all_six(#)==#(1);#(2);#(3);#(4);#(5);#(6)
@y
@d do_all_six(#)==#(1);#(2);#(3);#(4);#(5);#(6)
@d do_all_eight(#)==do_all_six(#);#(7);#(8)
  {the line breaker's six, and the stretch and shrink fonts may add}
@z

@x [38.833] - font expansion: the stretch and shrink of fonts in the widths
@!active_width:array[1..6] of scaled;
  {distance from first active node to~|cur_p|}
@!cur_active_width:array[1..6] of scaled; {distance from current active node}
@!background:array[1..6] of scaled; {length of an ``empty'' line}
@!break_width:array[1..6] of scaled; {length being computed after current break}
@y
@!active_width:array[1..8] of scaled;
  {distance from first active node to~|cur_p|}
@!cur_active_width:array[1..8] of scaled; {distance from current active node}
@!background:array[1..8] of scaled; {length of an ``empty'' line}
@!break_width:array[1..8] of scaled; {length being computed after current break}
@z

@x [38.827] - font expansion: the stretch and shrink of fonts in the widths
background[6]:=shrink(q)+shrink(r);
@y
background[6]:=shrink(q)+shrink(r);
background[7]:=0; background[8]:=0;
@z

@x [38.829] - font expansion: the stretch and shrink of fonts in the widths
no_break_yet:=true; prev_r:=active; old_l:=0;
do_all_six(copy_to_cur_active);
@y
no_break_yet:=true; prev_r:=active; old_l:=0;
do_all_eight(copy_to_cur_active);
@z

@x [38.832] - font expansion: the stretch and shrink of fonts in the widths
  begin do_all_six(update_width);
@y
  begin do_all_eight(update_width);
@z

@x [38.837] - font expansion: the stretch and shrink of fonts in the widths
begin no_break_yet:=false; do_all_six(set_break_width_to_background);
@y
begin no_break_yet:=false; do_all_eight(set_break_width_to_background);
@z

@x [38.839] - font expansion: the stretch and shrink of fonts in the widths
@!disc_width:scaled; {the length of discretionary material preceding a break}
@y
@!disc_width:scaled; {the length of discretionary material preceding a break}
@!disc_stretch,@!disc_shrink:scaled; {the stretch and shrink its fonts allow}
@z

@x [38.840] - font expansion: the stretch and shrink of fonts in the widths
break_width[1]:=break_width[1]+disc_width;
@y
break_width[1]:=break_width[1]+disc_width;
break_width[7]:=break_width[7]+disc_stretch;
break_width[8]:=break_width[8]+disc_shrink;
@z

@x [38.841] - font expansion: the stretch and shrink of fonts in the widths
  break_width[1]:=break_width[1]-char_width(f)(char_info(f)(character(v)));
  end
else  case type(v) of
  ligature_node: begin f:=font(lig_char(v));@/
    break_width[1]:=@|break_width[1]-
      char_width(f)(char_info(f)(character(lig_char(v))));
    end;
@y
  break_width[1]:=break_width[1]-char_width(f)(char_info(f)(character(v)));
  if pdf_adjust_spacing>1 then
    begin break_width[7]:=break_width[7]-char_stretch(f,character(v));
    break_width[8]:=break_width[8]-char_shrink(f,character(v));
    end;
  end
else  case type(v) of
  ligature_node: begin f:=font(lig_char(v));@/
    break_width[1]:=@|break_width[1]-
      char_width(f)(char_info(f)(character(lig_char(v))));
    if pdf_adjust_spacing>1 then
      begin break_width[7]:=break_width[7]-char_stretch(f,character(lig_char(v)));
      break_width[8]:=break_width[8]-char_shrink(f,character(lig_char(v)));
      end;
    end;
@z

@x [38.842] - font expansion: the stretch and shrink of fonts in the widths
  break_width[1]:=@|break_width[1]+char_width(f)(char_info(f)(character(s)));
  end
else  case type(s) of
  ligature_node: begin f:=font(lig_char(s));
    break_width[1]:=break_width[1]+
      char_width(f)(char_info(f)(character(lig_char(s))));
    end;
@y
  break_width[1]:=@|break_width[1]+char_width(f)(char_info(f)(character(s)));
  if pdf_adjust_spacing>1 then
    begin break_width[7]:=break_width[7]+char_stretch(f,character(s));
    break_width[8]:=break_width[8]+char_shrink(f,character(s));
    end;
  end
else  case type(s) of
  ligature_node: begin f:=font(lig_char(s));
    break_width[1]:=break_width[1]+
      char_width(f)(char_info(f)(character(lig_char(s))));
    if pdf_adjust_spacing>1 then
      begin break_width[7]:=break_width[7]+char_stretch(f,character(lig_char(s)));
      break_width[8]:=break_width[8]+char_shrink(f,character(lig_char(s)));
      end;
    end;
@z

@x [38.844] - font expansion: the stretch and shrink of fonts in the widths
  begin do_all_six(convert_to_break_width);
  end
else if prev_r=active then {no delta node needed at the beginning}
  begin do_all_six(store_break_width);
  end
else  begin q:=get_node(delta_node_size); link(q):=r; type(q):=delta_node;@/
  subtype(q):=0; {the |subtype| is not used}
  do_all_six(new_delta_to_break_width);
@y
  begin do_all_eight(convert_to_break_width);
  end
else if prev_r=active then {no delta node needed at the beginning}
  begin do_all_eight(store_break_width);
  end
else  begin q:=get_node(delta_node_size); link(q):=r; type(q):=delta_node;@/
  subtype(q):=0; {the |subtype| is not used}
  do_all_eight(new_delta_to_break_width);
@z

@x [38.845] - font expansion: the stretch and shrink of fonts in the widths
  do_all_six(new_delta_from_break_width);
@y
  do_all_eight(new_delta_from_break_width);
@z

@x [38.851] - character protrusion and font expansion in the line-breaking decisions
shortfall:=line_width-cur_active_width[1]; {we're this much too short}
@y
shortfall:=line_width-cur_active_width[1]; {we're this much too short}
if pdf_protrude_chars>1 then
  shortfall:=shortfall+line_protrusion(break_node(r),cur_p);
if (pdf_adjust_spacing>1)and(shortfall<>0) then
  shortfall:=expansion_shortfall(shortfall,cur_active_width[7],cur_active_width[8]);
@z

@x [38.860] - font expansion: the stretch and shrink of fonts in the widths
    begin do_all_six(downdate_width);
@y
    begin do_all_eight(downdate_width);
@z

@x [38.861] - font expansion: the stretch and shrink of fonts in the widths
  begin do_all_six(update_active);
  do_all_six(copy_to_cur_active);
@y
  begin do_all_eight(update_active);
  do_all_eight(copy_to_cur_active);
@z

@x [39.864] - font expansion: the stretch and shrink of fonts in the widths
do_all_six(store_background);@/
@y
do_all_eight(store_background);@/
@z

@x [39.866] - margin kerns of an unpacked line
kern_node: if subtype(cur_p)=explicit then kern_break
@y
margin_kern_node: act_width:=act_width+width(cur_p);
kern_node: if subtype(cur_p)=explicit then kern_break
@z

@x [39.866] - font expansion: the stretch and shrink of fonts in the widths
ligature_node: begin f:=font(lig_char(cur_p));
  act_width:=act_width+char_width(f)(char_info(f)(character(lig_char(cur_p))));
  end;
@y
ligature_node: begin f:=font(lig_char(cur_p));
  act_width:=act_width+char_width(f)(char_info(f)(character(lig_char(cur_p))));
  if pdf_adjust_spacing>1 then
    begin active_width[7]:=active_width[7]+char_stretch(f,character(lig_char(cur_p)));
    active_width[8]:=active_width[8]+char_shrink(f,character(lig_char(cur_p)));
    end;
  end;
@z

@x [39.867] - font expansion: the stretch and shrink of fonts in the widths
act_width:=act_width+char_width(f)(char_info(f)(character(cur_p)));
@y
act_width:=act_width+char_width(f)(char_info(f)(character(cur_p)));
if pdf_adjust_spacing>1 then
  begin active_width[7]:=active_width[7]+char_stretch(f,character(cur_p));
  active_width[8]:=active_width[8]+char_shrink(f,character(cur_p));
  end;
@z

@x [39.869] - font expansion: the stretch and shrink of fonts in the widths
begin s:=pre_break(cur_p); disc_width:=0;
@y
begin s:=pre_break(cur_p); disc_width:=0; disc_stretch:=0; disc_shrink:=0;
@z

@x [39.869] - font expansion: the stretch and shrink of fonts in the widths
  act_width:=act_width+disc_width;
  try_break(hyphen_penalty,hyphenated);
  act_width:=act_width-disc_width;
@y
  act_width:=act_width+disc_width;
  active_width[7]:=active_width[7]+disc_stretch;
  active_width[8]:=active_width[8]+disc_shrink;
  try_break(hyphen_penalty,hyphenated);
  act_width:=act_width-disc_width;
  active_width[7]:=active_width[7]-disc_stretch;
  active_width[8]:=active_width[8]-disc_shrink;
@z

@x [39.870] - font expansion: the stretch and shrink of fonts in the widths
  disc_width:=disc_width+char_width(f)(char_info(f)(character(s)));
  end
else  case type(s) of
  ligature_node: begin f:=font(lig_char(s));
    disc_width:=disc_width+
      char_width(f)(char_info(f)(character(lig_char(s))));
    end;
@y
  disc_width:=disc_width+char_width(f)(char_info(f)(character(s)));
  if pdf_adjust_spacing>1 then
    begin disc_stretch:=disc_stretch+char_stretch(f,character(s));
    disc_shrink:=disc_shrink+char_shrink(f,character(s));
    end;
  end
else  case type(s) of
  ligature_node: begin f:=font(lig_char(s));
    disc_width:=disc_width+
      char_width(f)(char_info(f)(character(lig_char(s))));
    if pdf_adjust_spacing>1 then
      begin disc_stretch:=disc_stretch+char_stretch(f,character(lig_char(s)));
      disc_shrink:=disc_shrink+char_shrink(f,character(lig_char(s)));
      end;
    end;
@z

@x [39.871] - font expansion: the stretch and shrink of fonts in the widths
  act_width:=act_width+char_width(f)(char_info(f)(character(s)));
  end
else  case type(s) of
  ligature_node: begin f:=font(lig_char(s));
    act_width:=act_width+
      char_width(f)(char_info(f)(character(lig_char(s))));
    end;
@y
  act_width:=act_width+char_width(f)(char_info(f)(character(s)));
  if pdf_adjust_spacing>1 then
    begin active_width[7]:=active_width[7]+char_stretch(f,character(s));
    active_width[8]:=active_width[8]+char_shrink(f,character(s));
    end;
  end
else  case type(s) of
  ligature_node: begin f:=font(lig_char(s));
    act_width:=act_width+
      char_width(f)(char_info(f)(character(lig_char(s))));
    if pdf_adjust_spacing>1 then
      begin active_width[7]:=active_width[7]+char_stretch(f,character(lig_char(s)));
      active_width[8]:=active_width[8]+char_shrink(f,character(lig_char(s)));
      end;
    end;
@z

@x [39.881] - the margin kern at the end of a line
  if type(q)=glue_node then
    begin delete_glue_ref(glue_ptr(q));
@y
  if type(q)=glue_node then
    begin if pdf_protrude_chars>0 then protrude_right(q,false);
    delete_glue_ref(glue_ptr(q));
@z

@x [39.881] - the margin kern at the end of a line
else  begin q:=temp_head;
  while link(q)<>null do q:=link(q);
  end;
@<Put the \(r)\.{\\rightskip} glue after node |q|@>;
@y
else  begin q:=temp_head;
  while link(q)<>null do q:=link(q);
  end;
if pdf_protrude_chars>0 then
  if disc_break and(is_char_node(q) or(type(q)<>disc_node)) then
    q:=protrude_right(q,true) {after the pre-break text}
  else q:=protrude_right(q,false);
@<Put the \(r)\.{\\rightskip} glue after node |q|@>;
@z

@x [39.887] - the margin kern at the start of a line
r:=link(q); link(q):=null; q:=link(temp_head); link(temp_head):=r;
@y
r:=link(q); link(q):=null; q:=link(temp_head); link(temp_head):=r;
if pdf_protrude_chars>0 then q:=protrude_left(q);
@z

@x [39.889] - font expansion of a line
adjust_tail:=adjust_head; just_box:=hpack(q,cur_width,exactly);
@y
adjust_tail:=adjust_head;
if pdf_adjust_spacing>0 then expand_line(q,cur_width);
just_box:=hpack(q,cur_width,exactly);
@z

@x [49.1253] - pdfTeX's font character codes
assign_font_int: begin n:=cur_chr; scan_font_ident; f:=cur_val;
  scan_optional_equals; scan_int;
  if n=0 then hyphen_char[f]:=cur_val@+else skew_char[f]:=cur_val;
  end;
@y
assign_font_int: begin n:=cur_chr; scan_font_ident; f:=cur_val;
  if n<2 then
    begin scan_optional_equals; scan_int;
    if n=0 then hyphen_char[f]:=cur_val@+else skew_char[f]:=cur_val;
    end
  else  begin scan_char_num; p:=cur_val; scan_optional_equals; scan_int;
    set_font_char_code(n,f,p,cur_val);
    end;
  end;
@z

@x [49.1263] - pdfTeX's font character codes
assign_font_int: if chr_code=0 then print_esc("hyphenchar")
  else print_esc("skewchar");
@y
assign_font_int: case chr_code of
  0: print_esc("hyphenchar");
  1: print_esc("skewchar");
  lp_code_base: print_esc("lpcode");
  rp_code_base: print_esc("rpcode");
  ef_code_base: print_esc("efcode");
  tag_code: print_esc("tagcode");
  kn_bs_code_base: print_esc("knbscode");
  st_bs_code_base: print_esc("stbscode");
  sh_bs_code_base: print_esc("shbscode");
  kn_bc_code_base: print_esc("knbccode");
  othercases print_esc("knaccode")
  endcases;
@z

@x [49.1210] - \letterspacefont and \pdfcopyfont, made as \font is
def_font: new_font(a);
@y
def_font: if cur_chr=0 then new_font(a) else new_font_copy(cur_chr=1,global);
@z

@x [50.1327] - pdfTeX's own state (\pdfglyphtounicode, \pdfmapline ...) is in the format
dump_int(interaction); dump_int(format_ident); dump_int(69069);
@y
pdf_dump_state;
dump_int(interaction); dump_int(format_ident); dump_int(69069);
@z

@x [50.1327] - pdfTeX's own state is in the format
undump(batch_mode)(error_stop_mode)(interaction);
@y
if not pdf_undump_state then goto bad_fmt;
undump(batch_mode)(error_stop_mode)(interaction);
@z

@x [51.1333] - the PDF file is finished instead of the DVI file
wake_up_terminal; @<Finish the \.{DVI} file@>;
@y
wake_up_terminal;
if pdf_output_used then pdf_finish_file
else begin @<Finish the \.{DVI} file@>; end;
@z

@x [53.1346] - pdfTeX's extensions
  othercases print("[unknown extension!]")
@y
  othercases print_pdf_extension(chr_code)
@z

@x [53.1348] - pdfTeX's extensions
othercases confusion("ext1")
@y
othercases do_pdf_extension(cur_chr)
@z

@x [53.1356] - pdfTeX's whatsits
othercases print("whatsit?")
@y
othercases display_pdf_whatsit(p)
@z

@x [53.1357] - pdfTeX's whatsits
othercases confusion("ext2")
@y
othercases r:=copy_pdf_whatsit(p,words)
@z

@x [53.1358] - pdfTeX's whatsits
othercases confusion("ext3")
@y
othercases free_pdf_whatsit(p)
@z

@x [53.1359] - image and form references have dimensions
@ @<Incorporate a whatsit node into a vbox@>=do_nothing
@y
@ @<Incorporate a whatsit node into a vbox@>=
if (subtype(p)=pdf_refxform_node)or(subtype(p)=pdf_refximage_node) then
  begin x:=x+d+height(p); d:=depth(p);
  if width(p)>w then w:=width(p);
  end
@z

@x [53.1360] - image and form references have dimensions
@ @<Incorporate a whatsit node into an hbox@>=do_nothing
@y
@ @<Incorporate a whatsit node into an hbox@>=
if (subtype(p)=pdf_refxform_node)or(subtype(p)=pdf_refximage_node) then
  begin x:=x+width(p);
  if height(p)>h then h:=height(p);
  if depth(p)>d then d:=depth(p);
  end
@z

@x [53.1361] - image and form references have dimensions
@ @<Let |d| be the width of the whatsit |p|@>=d:=0
@y
@ @<Let |d| be the width of the whatsit |p|@>=
if (subtype(p)=pdf_refxform_node)or(subtype(p)=pdf_refximage_node) then d:=width(p)
else d:=0
@z

@x [53.1362] - image and form references have dimensions
@<Advance \(p)past a whatsit node in the \(l)|line_break| loop@>=@+
adv_past(cur_p)
@y
@<Advance \(p)past a whatsit node in the \(l)|line_break| loop@>=@+
begin adv_past(cur_p);
if (subtype(cur_p)=pdf_refxform_node)or(subtype(cur_p)=pdf_refximage_node) then
  act_width:=act_width+width(cur_p);
end
@z

@x [53.1364] - image and form references have dimensions
@ @<Prepare to move whatsit |p| to the current page, then |goto contribute|@>=
goto contribute
@y
@ @<Prepare to move whatsit |p| to the current page, then |goto contribute|@>=
begin if (subtype(p)=pdf_refxform_node)or(subtype(p)=pdf_refximage_node) then
  begin page_total:=page_total+page_depth+height(p);
  page_depth:=depth(p);
  end;
goto contribute;
end
@z

@x [53.1365] - image and form references have dimensions
@ @<Process whatsit |p| in |vert_break| loop, |goto not_found|@>=
goto not_found
@y
@ @<Process whatsit |p| in |vert_break| loop, |goto not_found|@>=
begin if (subtype(p)=pdf_refxform_node)or(subtype(p)=pdf_refximage_node) then
  begin cur_height:=cur_height+prev_dp+height(p); prev_dp:=depth(p);
  end;
goto not_found;
end
@z

@x [53.1373] - pdfTeX's whatsits in DVI output
othercases confusion("ext4")
@y
othercases pdf_out_what_dvi(p)
@z

@x [53.1374] - \openout is logged, as TeX Live's engines do
      write_open[j]:=true;
@y
      write_open[j]:=true; log_openout(j);
@z

@x [53.1375] - \immediate\pdfobj and friends
if (cur_cmd=extension)and(cur_chr<=close_node) then
  begin p:=tail; do_extension; {append a whatsit node}
  out_what(tail); {do the action immediately}
  flush_node_list(tail); tail:=p; link(p):=null;
  end
else back_input;
@y
if (cur_cmd=extension)and(cur_chr<=close_node) then
  begin p:=tail; do_extension; {append a whatsit node}
  out_what(tail); {do the action immediately}
  flush_node_list(tail); tail:=p; link(p):=null;
  end
else if (cur_cmd=extension)and pdf_immediate_ok(cur_chr) then
  pdf_immediate(cur_chr)
else back_input;
@z

@x [54/e-TeX] - pdfTeX's new sections
@ @<Declare act...@>=
@<Declare \eTeX\ procedures for scanning@>@;
@y
@ @<Declare act...@>=
@<Declare \eTeX\ procedures for scanning@>@;

@* \[54/pdfTeX] The pdfTeX extensions.
pdfTeX's command codes and primitives. The work is done in
\.{tex\_pdf.cpp}.

@d pdf_first_convert_code=eTeX_revision_code+1 {first convert code of pdfTeX}
@d pdf_strcmp_code=pdf_first_convert_code {\.{\\pdfstrcmp}}
@d expanded_code=pdf_first_convert_code+1 {\.{\\expanded}}
@d pdftex_revision_code=pdf_first_convert_code+2 {\.{\\pdftexrevision}}
@d pdftex_banner_code=pdf_first_convert_code+3 {\.{\\pdftexbanner}}
@d pdf_font_name_code=pdf_first_convert_code+4 {\.{\\pdffontname}}
@d pdf_font_objnum_code=pdf_first_convert_code+5 {\.{\\pdffontobjnum}}
@d pdf_font_size_code=pdf_first_convert_code+6 {\.{\\pdffontsize}}
@d pdf_page_ref_code=pdf_first_convert_code+7 {\.{\\pdfpageref}}
@d pdf_xform_name_code=pdf_first_convert_code+8 {\.{\\pdfxformname}}
@d pdf_escape_string_code=pdf_first_convert_code+9 {\.{\\pdfescapestring}}
@d pdf_escape_name_code=pdf_first_convert_code+10 {\.{\\pdfescapename}}
@d pdf_escape_hex_code=pdf_first_convert_code+11 {\.{\\pdfescapehex}}
@d pdf_unescape_hex_code=pdf_first_convert_code+12 {\.{\\pdfunescapehex}}
@d pdf_creation_date_code=pdf_first_convert_code+13 {\.{\\pdfcreationdate}}
@d pdf_file_mod_date_code=pdf_first_convert_code+14 {\.{\\pdffilemoddate}}
@d pdf_file_size_code=pdf_first_convert_code+15 {\.{\\pdffilesize}}
@d pdf_mdfive_sum_code=pdf_first_convert_code+16 {\.{\\pdfmdfivesum}}
@d pdf_file_dump_code=pdf_first_convert_code+17 {\.{\\pdffiledump}}
@d pdf_match_code=pdf_first_convert_code+18 {\.{\\pdfmatch}}
@d pdf_last_match_code=pdf_first_convert_code+19 {\.{\\pdflastmatch}}
@d pdf_uniform_deviate_code=pdf_first_convert_code+20 {\.{\\pdfuniformdeviate}}
@d pdf_normal_deviate_code=pdf_first_convert_code+21 {\.{\\pdfnormaldeviate}}
@d pdf_colorstack_init_code=pdf_first_convert_code+22 {\.{\\pdfcolorstackinit}}
@d left_margin_kern_code=pdf_first_convert_code+23 {\.{\\leftmarginkern}}
@d right_margin_kern_code=pdf_first_convert_code+24 {\.{\\rightmarginkern}}
@d pdf_ximage_bbox_code=pdf_first_convert_code+25 {\.{\\pdfximagebbox}}
@d pdf_insert_ht_code=pdf_first_convert_code+26 {\.{\\pdfinsertht}}
@#
@d pdftex_version_code=mu_expr_code+1 {\.{\\pdftexversion}}
@d pdf_last_obj_code=mu_expr_code+2 {\.{\\pdflastobj}}
@d pdf_last_xform_code=mu_expr_code+3 {\.{\\pdflastxform}}
@d pdf_last_ximage_code=mu_expr_code+4 {\.{\\pdflastximage}}
@d pdf_last_ximage_pages_code=mu_expr_code+5 {\.{\\pdflastximagepages}}
@d pdf_last_annot_code=mu_expr_code+6 {\.{\\pdflastannot}}
@d pdf_last_link_code=mu_expr_code+7 {\.{\\pdflastlink}}
@d pdf_last_x_pos_code=mu_expr_code+8 {\.{\\pdflastxpos}}
@d pdf_last_y_pos_code=mu_expr_code+9 {\.{\\pdflastypos}}
@d pdf_retval_code=mu_expr_code+10 {\.{\\pdfretval}}
@d pdf_last_ximage_colordepth_code=mu_expr_code+11 {\.{\\pdflastximagecolordepth}}
@d elapsed_time_code=mu_expr_code+12 {\.{\\pdfelapsedtime}}
@d pdf_shell_escape_code=mu_expr_code+13 {\.{\\pdfshellescape}}
@d random_seed_code=mu_expr_code+14 {\.{\\pdfrandomseed}}
@d meptex_version_code=mu_expr_code+15 {\.{\\meptexversion}: mep-tex, not pdfTeX}
@#
@d pdf_literal_node=6 {|subtype| of \.{\\pdfliteral}, and its |extension| code}
@d pdf_refobj_node=7 {\.{\\pdfrefobj}}
@d pdf_refxform_node=8 {\.{\\pdfrefxform}}
@d pdf_refximage_node=9 {\.{\\pdfrefximage}}
@d pdf_annot_node=10 {\.{\\pdfannot}}
@d pdf_start_link_node=11 {\.{\\pdfstartlink}}
@d pdf_end_link_node=12 {\.{\\pdfendlink}}
@d pdf_dest_node=13 {\.{\\pdfdest}}
@d pdf_thread_node=14 {\.{\\pdfthread}}
@d pdf_start_thread_node=15 {\.{\\pdfstartthread}}
@d pdf_end_thread_node=16 {\.{\\pdfendthread}}
@d pdf_save_pos_node=17 {\.{\\pdfsavepos}}
@d pdf_colorstack_node=18 {\.{\\pdfcolorstack}}
@d pdf_setmatrix_node=19 {\.{\\pdfsetmatrix}}
@d pdf_save_node=20 {\.{\\pdfsave}}
@d pdf_restore_node=21 {\.{\\pdfrestore}}
@d pdf_snap_ref_point_node=22 {\.{\\pdfsnaprefpoint}}
@d pdf_snapy_node=23 {\.{\\pdfsnapy}}
@d pdf_snapy_comp_node=24 {\.{\\pdfsnapycomp}}
@d pdf_obj_code=40 {\.{\\pdfobj}: |extension| codes from here on make no node}
@d pdf_xform_code=41 {\.{\\pdfxform}}
@d pdf_ximage_code=42 {\.{\\pdfximage}}
@d pdf_outline_code=43 {\.{\\pdfoutline}}
@d pdf_info_code=44 {\.{\\pdfinfo}}
@d pdf_catalog_code=45 {\.{\\pdfcatalog}}
@d pdf_names_code=46 {\.{\\pdfnames}}
@d pdf_trailer_code=47 {\.{\\pdftrailer}}
@d pdf_trailer_id_code=48 {\.{\\pdftrailerid}}
@d pdf_map_file_code=49 {\.{\\pdfmapfile}}
@d pdf_map_line_code=50 {\.{\\pdfmapline}}
@d pdf_font_attr_code=51 {\.{\\pdffontattr}}
@d pdf_font_expand_code=52 {\.{\\pdffontexpand}}
@d pdf_include_chars_code=53 {\.{\\pdfincludechars}}
@d pdf_glyph_to_unicode_code=54 {\.{\\pdfglyphtounicode}}
@d pdf_nobuiltin_tounicode_code=55 {\.{\\pdfnobuiltintounicode}}
@d pdf_no_ligatures_code=56 {\.{\\pdfnoligatures}}
@d pdf_interword_space_on_code=57 {\.{\\pdfinterwordspaceon}}
@d pdf_interword_space_off_code=58 {\.{\\pdfinterwordspaceoff}}
@d pdf_fake_space_code=59 {\.{\\pdffakespace}}
@d pdf_space_font_code=60 {\.{\\pdfspacefont}}
@d pdf_running_link_off_code=61 {\.{\\pdfrunninglinkoff}}
@d pdf_running_link_on_code=62 {\.{\\pdfrunninglinkon}}
@d pdf_reset_timer_code=63 {\.{\\pdfresettimer}}
@d pdf_set_random_seed_code=64 {\.{\\pdfsetrandomseed}}
@d quitvmode_code=65 {\.{\\quitvmode}}
@d partokenname_code=66 {\.{\\partokenname}}
@d letterspace_font_code=67 {\.{\\letterspacefont}}
@d pdf_copy_font_code=68 {\.{\\pdfcopyfont}}
@d pdf_primitive_code=69 {unused}
@#
@d lp_code_base=2 {|assign_font_int| codes: \.{\\lpcode}}
@d rp_code_base=3 {\.{\\rpcode}}
@d ef_code_base=4 {\.{\\efcode}}
@d tag_code=5 {\.{\\tagcode}}
@d kn_bs_code_base=6 {\.{\\knbscode}}
@d st_bs_code_base=7 {\.{\\stbscode}}
@d sh_bs_code_base=8 {\.{\\shbscode}}
@d kn_bc_code_base=9 {\.{\\knbccode}}
@d kn_ac_code_base=10 {\.{\\knaccode}}

@<Glob...@>=
@!incsname_state:integer; {inside \.{\\csname}, \.{\\ifcsname}: for \.{\\ifincsname}}

@ @<Set init...@>=
incsname_state:=0;

@ @<Put each...@>=
primitive("pdfoutput",assign_int,int_base+pdf_output_code);@/
primitive("pdfcompresslevel",assign_int,int_base+pdf_compress_level_code);@/
primitive("pdfdecimaldigits",assign_int,int_base+pdf_decimal_digits_code);@/
primitive("pdfmovechars",assign_int,int_base+pdf_move_chars_code);@/
primitive("pdfimageresolution",assign_int,int_base+pdf_image_resolution_code);@/
primitive("pdfpkresolution",assign_int,int_base+pdf_pk_resolution_code);@/
primitive("pdfuniqueresname",assign_int,int_base+pdf_unique_resname_code);@/
primitive("pdfforcepagebox",assign_int,int_base+pdf_force_pagebox_code);@/
primitive("pdfoptionalwaysusepdfpagebox",assign_int,int_base+pdf_force_pagebox_code);@/
primitive("pdfinclusionerrorlevel",assign_int,int_base+pdf_inclusion_errorlevel_code);@/
primitive("pdfoptionpdfinclusionerrorlevel",assign_int,int_base+pdf_inclusion_errorlevel_code);@/
primitive("pdfmajorversion",assign_int,int_base+pdf_major_version_code);@/
primitive("pdfminorversion",assign_int,int_base+pdf_minor_version_code);@/
primitive("pdfoptionpdfminorversion",assign_int,int_base+pdf_minor_version_code);@/
primitive("pdfpagebox",assign_int,int_base+pdf_page_box_code);@/
primitive("pdfgamma",assign_int,int_base+pdf_gamma_code);@/
primitive("pdfimagegamma",assign_int,int_base+pdf_image_gamma_code);@/
primitive("pdfimagehicolor",assign_int,int_base+pdf_image_hicolor_code);@/
primitive("pdfimageapplygamma",assign_int,int_base+pdf_image_apply_gamma_code);@/
primitive("pdfadjustspacing",assign_int,int_base+pdf_adjust_spacing_code);@/
primitive("pdfprotrudechars",assign_int,int_base+pdf_protrude_chars_code);@/
primitive("pdftracingfonts",assign_int,int_base+pdf_tracing_fonts_code);@/
primitive("pdfobjcompresslevel",assign_int,int_base+pdf_obj_compress_level_code);@/
primitive("pdfadjustinterwordglue",assign_int,int_base+pdf_adjust_interword_glue_code);@/
primitive("pdfprependkern",assign_int,int_base+pdf_prepend_kern_code);@/
primitive("pdfappendkern",assign_int,int_base+pdf_append_kern_code);@/
primitive("pdfgentounicode",assign_int,int_base+pdf_gen_tounicode_code);@/
primitive("pdfdraftmode",assign_int,int_base+pdf_draftmode_code);@/
primitive("pdfinclusioncopyfonts",assign_int,int_base+pdf_inclusion_copy_font_code);@/
primitive("pdfsuppresswarningdupdest",assign_int,int_base+pdf_suppress_warning_dup_dest_code);@/
primitive("pdfsuppresswarningdupmap",assign_int,int_base+pdf_suppress_warning_dup_map_code);@/
primitive("pdfsuppresswarningpagegroup",assign_int,int_base+pdf_suppress_warning_page_group_code);@/
primitive("pdfinfoomitdate",assign_int,int_base+pdf_info_omit_date_code);@/
primitive("pdfsuppressptexinfo",assign_int,int_base+pdf_suppress_ptex_info_code);@/
primitive("pdfomitcharset",assign_int,int_base+pdf_omit_charset_code);@/
primitive("pdfomitinfodict",assign_int,int_base+pdf_omit_info_dict_code);@/
primitive("pdfomitprocset",assign_int,int_base+pdf_omit_procset_code);@/
primitive("showstream",assign_int,int_base+show_stream_code);@/
primitive("tracingstacklevels",assign_int,int_base+tracing_stack_levels_code);@/
primitive("partokencontext",assign_int,int_base+partoken_context_code);@/
primitive("ignoreprimitiveerror",assign_int,int_base+ignore_primitive_error_code);@/
primitive("pdfhorigin",assign_dimen,dimen_base+pdf_h_origin_code);@/
primitive("pdfvorigin",assign_dimen,dimen_base+pdf_v_origin_code);@/
primitive("pdfpagewidth",assign_dimen,dimen_base+pdf_page_width_code);@/
primitive("pdfpageheight",assign_dimen,dimen_base+pdf_page_height_code);@/
primitive("pdflinkmargin",assign_dimen,dimen_base+pdf_link_margin_code);@/
primitive("pdfdestmargin",assign_dimen,dimen_base+pdf_dest_margin_code);@/
primitive("pdfthreadmargin",assign_dimen,dimen_base+pdf_thread_margin_code);@/
primitive("pdffirstlineheight",assign_dimen,dimen_base+pdf_first_line_height_code);@/
primitive("pdflastlinedepth",assign_dimen,dimen_base+pdf_last_line_depth_code);@/
primitive("pdfeachlineheight",assign_dimen,dimen_base+pdf_each_line_height_code);@/
primitive("pdfeachlinedepth",assign_dimen,dimen_base+pdf_each_line_depth_code);@/
primitive("pdfignoreddimen",assign_dimen,dimen_base+pdf_ignored_dimen_code);@/
primitive("pdfpxdimen",assign_dimen,dimen_base+pdf_px_dimen_code);@/
primitive("pdfpagesattr",assign_toks,pdf_pages_attr_loc);@/
primitive("pdfpageattr",assign_toks,pdf_page_attr_loc);@/
primitive("pdfpageresources",assign_toks,pdf_page_resources_loc);@/
primitive("pdfpkmode",assign_toks,pdf_pk_mode_loc);@/
primitive("pdfstrcmp",convert,pdf_strcmp_code);@/
primitive("expanded",convert,expanded_code);@/
primitive("pdftexrevision",convert,pdftex_revision_code);@/
primitive("pdftexbanner",convert,pdftex_banner_code);@/
primitive("pdffontname",convert,pdf_font_name_code);@/
primitive("pdffontobjnum",convert,pdf_font_objnum_code);@/
primitive("pdffontsize",convert,pdf_font_size_code);@/
primitive("pdfpageref",convert,pdf_page_ref_code);@/
primitive("pdfxformname",convert,pdf_xform_name_code);@/
primitive("pdfescapestring",convert,pdf_escape_string_code);@/
primitive("pdfescapename",convert,pdf_escape_name_code);@/
primitive("pdfescapehex",convert,pdf_escape_hex_code);@/
primitive("pdfunescapehex",convert,pdf_unescape_hex_code);@/
primitive("pdfcreationdate",convert,pdf_creation_date_code);@/
primitive("pdffilemoddate",convert,pdf_file_mod_date_code);@/
primitive("pdffilesize",convert,pdf_file_size_code);@/
primitive("pdfmdfivesum",convert,pdf_mdfive_sum_code);@/
primitive("pdffiledump",convert,pdf_file_dump_code);@/
primitive("pdfmatch",convert,pdf_match_code);@/
primitive("pdflastmatch",convert,pdf_last_match_code);@/
primitive("pdfuniformdeviate",convert,pdf_uniform_deviate_code);@/
primitive("pdfnormaldeviate",convert,pdf_normal_deviate_code);@/
primitive("pdfcolorstackinit",convert,pdf_colorstack_init_code);@/
primitive("leftmarginkern",convert,left_margin_kern_code);@/
primitive("rightmarginkern",convert,right_margin_kern_code);@/
primitive("pdfximagebbox",convert,pdf_ximage_bbox_code);@/
primitive("pdfinsertht",convert,pdf_insert_ht_code);@/
primitive("pdftexversion",last_item,pdftex_version_code);@/
primitive("pdflastobj",last_item,pdf_last_obj_code);@/
primitive("pdflastxform",last_item,pdf_last_xform_code);@/
primitive("pdflastximage",last_item,pdf_last_ximage_code);@/
primitive("pdflastximagepages",last_item,pdf_last_ximage_pages_code);@/
primitive("pdflastannot",last_item,pdf_last_annot_code);@/
primitive("pdflastlink",last_item,pdf_last_link_code);@/
primitive("pdflastxpos",last_item,pdf_last_x_pos_code);@/
primitive("pdflastypos",last_item,pdf_last_y_pos_code);@/
primitive("pdfretval",last_item,pdf_retval_code);@/
primitive("pdflastximagecolordepth",last_item,pdf_last_ximage_colordepth_code);@/
primitive("pdfelapsedtime",last_item,elapsed_time_code);@/
primitive("pdfshellescape",last_item,pdf_shell_escape_code);@/
primitive("pdfrandomseed",last_item,random_seed_code);@/
primitive("meptexversion",last_item,meptex_version_code);@/
primitive("pdfprimitive",expand_after,2);@/
primitive("ifincsname",if_test,if_in_csname_code);@/
primitive("ifpdfprimitive",if_test,if_pdfprimitive_code);@/
primitive("ifpdfabsnum",if_test,if_abs_num_code);@/
primitive("ifpdfabsdim",if_test,if_abs_dim_code);@/
primitive("pdfliteral",extension,pdf_literal_node);@/
primitive("pdfrefobj",extension,pdf_refobj_node);@/
primitive("pdfrefxform",extension,pdf_refxform_node);@/
primitive("pdfrefximage",extension,pdf_refximage_node);@/
primitive("pdfannot",extension,pdf_annot_node);@/
primitive("pdfstartlink",extension,pdf_start_link_node);@/
primitive("pdfendlink",extension,pdf_end_link_node);@/
primitive("pdfdest",extension,pdf_dest_node);@/
primitive("pdfthread",extension,pdf_thread_node);@/
primitive("pdfstartthread",extension,pdf_start_thread_node);@/
primitive("pdfendthread",extension,pdf_end_thread_node);@/
primitive("pdfsavepos",extension,pdf_save_pos_node);@/
primitive("pdfcolorstack",extension,pdf_colorstack_node);@/
primitive("pdfsetmatrix",extension,pdf_setmatrix_node);@/
primitive("pdfsave",extension,pdf_save_node);@/
primitive("pdfrestore",extension,pdf_restore_node);@/
primitive("pdfsnaprefpoint",extension,pdf_snap_ref_point_node);@/
primitive("pdfsnapy",extension,pdf_snapy_node);@/
primitive("pdfsnapycomp",extension,pdf_snapy_comp_node);@/
primitive("pdfobj",extension,pdf_obj_code);@/
primitive("pdfxform",extension,pdf_xform_code);@/
primitive("pdfximage",extension,pdf_ximage_code);@/
primitive("pdfoutline",extension,pdf_outline_code);@/
primitive("pdfinfo",extension,pdf_info_code);@/
primitive("pdfcatalog",extension,pdf_catalog_code);@/
primitive("pdfnames",extension,pdf_names_code);@/
primitive("pdftrailer",extension,pdf_trailer_code);@/
primitive("pdftrailerid",extension,pdf_trailer_id_code);@/
primitive("pdfmapfile",extension,pdf_map_file_code);@/
primitive("pdfmapline",extension,pdf_map_line_code);@/
primitive("pdffontattr",extension,pdf_font_attr_code);@/
primitive("pdffontexpand",extension,pdf_font_expand_code);@/
primitive("pdfincludechars",extension,pdf_include_chars_code);@/
primitive("pdfglyphtounicode",extension,pdf_glyph_to_unicode_code);@/
primitive("pdfnobuiltintounicode",extension,pdf_nobuiltin_tounicode_code);@/
primitive("pdfnoligatures",extension,pdf_no_ligatures_code);@/
primitive("pdfinterwordspaceon",extension,pdf_interword_space_on_code);@/
primitive("pdfinterwordspaceoff",extension,pdf_interword_space_off_code);@/
primitive("pdffakespace",extension,pdf_fake_space_code);@/
primitive("pdfspacefont",extension,pdf_space_font_code);@/
primitive("pdfrunninglinkoff",extension,pdf_running_link_off_code);@/
primitive("pdfrunninglinkon",extension,pdf_running_link_on_code);@/
primitive("pdfresettimer",extension,pdf_reset_timer_code);@/
primitive("pdfsetrandomseed",extension,pdf_set_random_seed_code);@/
primitive("quitvmode",extension,quitvmode_code);@/
primitive("partokenname",extension,partokenname_code);@/
primitive("letterspacefont",def_font,1); {like \.{\\font}, these take \.{\\global}}
primitive("pdfcopyfont",def_font,2);@/
primitive("lpcode",assign_font_int,lp_code_base);@/
primitive("rpcode",assign_font_int,rp_code_base);@/
primitive("efcode",assign_font_int,ef_code_base);@/
primitive("tagcode",assign_font_int,tag_code);@/
primitive("knbscode",assign_font_int,kn_bs_code_base);@/
primitive("stbscode",assign_font_int,st_bs_code_base);@/
primitive("shbscode",assign_font_int,sh_bs_code_base);@/
primitive("knbccode",assign_font_int,kn_bc_code_base);@/
primitive("knaccode",assign_font_int,kn_ac_code_base);@/

@z

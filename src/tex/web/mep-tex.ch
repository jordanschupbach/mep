% mep-tex.ch -- the system-dependent parts of tex.web, for mep's TeX engine.
%
% A WEB change file in the usual @x/@y/@z form, applied to Knuth's tex.web
% (third_party/tex/tex.web) by tools/web2cpp before it translates the
% program to C++ (src/tex/gen/). Like web2c's tex.ch, it replaces what the
% \ph\ Pascal dialect of tex.web assumed about files, the terminal, the
% character set and memory with what mep provides; the C++ it calls is
% hand-written in src/tex/tex_system.cpp, reached either as routines this
% file names or as verbatim `@=...@>' text.
%
% Written for mep (MIT licence). TeX itself is copyright D. E. Knuth; a
% program built from tex.web with changes is not `TeX', which is why this
% one calls itself mep-tex.

@x [1.2] l.187 - banner
@d banner=='This is TeX, Version 3.141592653' {printed when \TeX\ starts}
@y
@d banner=='This is mep-tex, Version 3.141592653' {printed when \TeX\ starts}
@z

@x [1.7] l.309 - statistics are always gathered
@d stat==@{ {change this to `$\\{stat}\equiv\null$' when gathering
  usage statistics}
@d tats==@t@>@} {change this to `$\\{tats}\equiv\null$' when gathering
  usage statistics}
@y
@d stat== {statistics are always gathered}
@d tats==
@z

@x [1.8] l.331 - one program is both INITEX and the production version
@!init @<Initialize table entries (done by \.{INITEX} only)@>@;@+tini
@y
if ini_version then
  begin @<Initialize table entries (done by \.{INITEX} only)@>@;
  end;
@z

@x [1.11] l.363 - capacities are set at run time
@<Constants...@>=
@!mem_max=30000; {greatest index in \TeX's internal |mem| array;
  must be strictly less than |max_halfword|;
  must be equal to |mem_top| in \.{INITEX}, otherwise |>=mem_top|}
@!mem_min=0; {smallest index in \TeX's internal |mem| array;
  must be |min_halfword| or more;
  must be equal to |mem_bot| in \.{INITEX}, otherwise |<=mem_bot|}
@!buf_size=500; {maximum number of characters simultaneously present in
  current lines of open files and in control sequences between
  \.{\\csname} and \.{\\endcsname}; must not exceed |max_halfword|}
@!error_line=72; {width of context lines on terminal error messages}
@!half_error_line=42; {width of first lines of contexts in terminal
  error messages; should be between 30 and |error_line-15|}
@!max_print_line=79; {width of longest text lines output; should be at least 60}
@!stack_size=200; {maximum number of simultaneous input sources}
@!max_in_open=6; {maximum number of input files and error insertions that
  can be going on simultaneously}
@!font_max=75; {maximum internal font number; must not exceed |max_quarterword|
  and must be at most |font_base+256|}
@!font_mem_size=20000; {number of words of |font_info| for all fonts}
@!param_size=60; {maximum number of simultaneous macro parameters}
@!nest_size=40; {maximum number of semantic levels simultaneously active}
@!max_strings=3000; {maximum number of strings; must not exceed |max_halfword|}
@!string_vacancies=8000; {the minimum number of characters that should be
  available for the user's control sequences and font names,
  after \TeX's own error messages are stored}
@!pool_size=32000; {maximum number of characters in strings, including all
  error messages and help texts, and the names of all fonts and
  control sequences; must exceed |string_vacancies| by the total
  length of \TeX's own strings, which is currently about 23000}
@!save_size=600; {space for saving values outside of current group; must be
  at most |max_halfword|}
@!trie_size=8000; {space for hyphenation patterns; should be larger for
  \.{INITEX} than it is in production versions of \TeX}
@!trie_op_size=500; {space for ``opcodes'' in the hyphenation patterns}
@!dvi_buf_size=800; {size of the output buffer; must be a multiple of 8}
@!file_name_size=40; {file names shouldn't be longer than this}
@!pool_name='TeXformats:TEX.POOL                     ';
  {string of length |file_name_size|; tells where the string pool appears}
@y
@<Constants...@>=
@!font_max=9000; {maximum internal font number; must not exceed
  |max_quarterword|; it sizes a region of |eqtb|, so it is fixed here}
@!trie_op_size=35111; {space for ``opcodes'' in the hyphenation patterns;
  must not exceed |max_quarterword|}

@ The rest of \TeX's capacity is set when the program starts, not when it
is compiled: these are variables, which the hand-written start-up code
(|set_capacities|, in \.{tex\_system.cpp}) fills in before |alloc_arrays|
gives the arrays their sizes.

@<Glob...@>=
@!mem_max:integer; {greatest index in \TeX's internal |mem| array}
@!mem_min:integer; {smallest index in \TeX's internal |mem| array}
@!mem_top:integer; {largest index in the |mem| array dumped by \.{INITEX}}
@!buf_size:integer; {maximum number of characters simultaneously present in
  current lines of open files and in control sequences}
@!error_line:integer; {width of context lines on terminal error messages}
@!half_error_line:integer; {width of first lines of contexts in terminal
  error messages}
@!max_print_line:integer; {width of longest text lines output}
@!stack_size:integer; {maximum number of simultaneous input sources}
@!max_in_open:integer; {maximum number of input files and error insertions}
@!font_mem_size:integer; {number of words of |font_info| for all fonts}
@!param_size:integer; {maximum number of simultaneous macro parameters}
@!nest_size:integer; {maximum number of semantic levels simultaneously active}
@!max_strings:integer; {maximum number of strings}
@!string_vacancies:integer; {the minimum number of characters that should be
  available for the user's control sequences and font names}
@!pool_size:integer; {maximum number of characters in strings}
@!save_size:integer; {space for saving values outside of current group}
@!trie_size:integer; {space for hyphenation patterns}
@!dvi_buf_size:integer; {size of the output buffer; must be a multiple of 8}
@!file_name_size:integer; {file names shouldn't be longer than this}
@!ini_version:boolean; {are we \.{INITEX}?}
@!printable:packed array [0..255] of boolean;
  {which characters print as themselves rather than in \.{\^\^} form}
@z

@x [1.12] l.396 - mem_top is a variable; bigger tables
@d mem_top==30000 {largest index in the |mem| array dumped by \.{INITEX};
  must be substantially larger than |mem_bot|
  and not greater than |mem_max|}
@d font_base=0 {smallest internal font number; must not be less
  than |min_quarterword|}
@d hash_size=2100 {maximum number of control sequences; it should be at most
  about |(mem_max-mem_min)/10|}
@d hash_prime=1777 {a prime number equal to about 85\pct! of |hash_size|}
@d hyph_size=307 {another prime; the number of \.{\\hyphenation} exceptions}
@y
@d font_base=0 {smallest internal font number; must not be less
  than |min_quarterword|}
@d hash_size=524288 {maximum number of control sequences}
@d hash_prime=445633 {a prime number equal to about 85\pct! of |hash_size|}
@d hyph_size=8191 {another prime; the number of \.{\\hyphenation} exceptions}
@z

@x [2.23] l.695 - every character code stands for itself
for i:=0 to @'37 do xchr[i]:=' ';
for i:=@'177 to @'377 do xchr[i]:=' ';
@y
for i:=0 to @'37 do xchr[i]:=chr(i);
for i:=@'177 to @'377 do xchr[i]:=chr(i);
@z

@x [3.27] l.782 - opening files goes through mep's resolver
@d reset_OK(#)==erstat(#)=0
@d rewrite_OK(#)==erstat(#)=0

@p function a_open_in(var f:alpha_file):boolean;
  {open a text file for input}
begin reset(f,name_of_file,'/O'); a_open_in:=reset_OK(f);
end;
@#
function a_open_out(var f:alpha_file):boolean;
  {open a text file for output}
begin rewrite(f,name_of_file,'/O'); a_open_out:=rewrite_OK(f);
end;
@#
function b_open_in(var f:byte_file):boolean;
  {open a binary file for input}
begin reset(f,name_of_file,'/O'); b_open_in:=reset_OK(f);
end;
@#
function b_open_out(var f:byte_file):boolean;
  {open a binary file for output}
begin rewrite(f,name_of_file,'/O'); b_open_out:=rewrite_OK(f);
end;
@#
function w_open_in(var f:word_file):boolean;
  {open a word file for input}
begin reset(f,name_of_file,'/O'); w_open_in:=reset_OK(f);
end;
@#
function w_open_out(var f:word_file):boolean;
  {open a word file for output}
begin rewrite(f,name_of_file,'/O'); w_open_out:=rewrite_OK(f);
end;
@y
@d tex_kind=1 {a file read by \.{\\input} or \.{\\openin}}
@d tfm_kind=2 {a font metric file}
@d fmt_kind=3 {a format file}

@p function a_open_in(var f:alpha_file; @!kind:integer):boolean;
  {open a text file for input, searched for as a file of |kind|}
begin a_open_in:=open_in_file(f,kind);
end;
@#
function a_open_out(var f:alpha_file):boolean;
  {open a text file for output}
begin a_open_out:=open_out_file(f);
end;
@#
function b_open_in(var f:byte_file; @!kind:integer):boolean;
  {open a binary file for input}
begin b_open_in:=open_in_file(f,kind);
end;
@#
function b_open_out(var f:byte_file):boolean;
  {open a binary file for output}
begin b_open_out:=open_out_file(f);
end;
@#
function w_open_in(var f:word_file):boolean;
  {open a word file for input}
begin w_open_in:=open_in_file(f,fmt_kind);
end;
@#
function w_open_out(var f:word_file):boolean;
  {open a word file for output}
begin w_open_out:=open_out_file(f);
end;
@z

@x [3.31] l.869 - reading a line
@p function input_ln(var f:alpha_file;@!bypass_eoln:boolean):boolean;
  {inputs the next line or returns |false|}
var last_nonblank:0..buf_size; {|last| with trailing blanks removed}
begin if bypass_eoln then if not eof(f) then get(f);
  {input the first character of the line into |f^|}
last:=first; {cf.\ Matthew 19\thinspace:\thinspace30}
if eof(f) then input_ln:=false
else  begin last_nonblank:=first;
  while not eoln(f) do
    begin if last>=max_buf_stack then
      begin max_buf_stack:=last+1;
      if max_buf_stack=buf_size then
        @<Report overflow of the input buffer, and abort@>;
      end;
    buffer[last]:=xord[f^]; get(f); incr(last);
    if buffer[last-1]<>" " then last_nonblank:=last;
    end;
  last:=last_nonblank; input_ln:=true;
  end;
end;
@y
@p function input_ln(var f:alpha_file;@!bypass_eoln:boolean):boolean;
  {inputs the next line or returns |false|}
label done;
var last_nonblank:0..buf_size; {|last| with trailing blanks removed}
@!c:integer; {the next character of the line, or $-1$ at its end}
begin last:=first; {cf.\ Matthew 19\thinspace:\thinspace30}
if @=f.at_eof()@> then input_ln:=false
else  begin last_nonblank:=first;
  loop@+begin c:=@=f.next_char()@>;
    if c<0 then goto done;
    if last>=max_buf_stack then
      begin max_buf_stack:=last+1;
      if max_buf_stack=buf_size then
        @<Report overflow of the input buffer, and abort@>;
      end;
    buffer[last]:=xord[c]; incr(last);
    if (buffer[last-1]<>" ")and(buffer[last-1]<>@'11) then
      last_nonblank:=last; {as in \TeX\ Live, trailing tabs go too}
    end;
  done: last:=last_nonblank; input_ln:=true;
  end;
end;
@z

@x [3.33] l.927 - the terminal
@d t_open_in==reset(term_in,'TTY:','/O/I') {open the terminal for text input}
@d t_open_out==rewrite(term_out,'TTY:','/O') {open the terminal for text output}
@y
@d t_open_in==@=term_in.open_stdin(); load_command_line();@>
  {open the terminal for text input; the command line is its first line}
@d t_open_out==open_terminal {open the terminal for text output}
@z

@x [3.34] l.946 - synchronizing the terminal
@d update_terminal == break(term_out) {empty the terminal output buffer}
@d clear_terminal == break_in(term_in,true) {clear the terminal input buffer}
@y
@d update_terminal == @=term_out.flush();@> {empty the terminal output buffer}
@d clear_terminal == do_nothing {clear the terminal input buffer}
@z

@x [3.37] l.974 - the command line is the first line
@p function init_terminal:boolean; {gets the terminal input started}
label exit;
begin t_open_in;
@y
@p function init_terminal:boolean; {gets the terminal input started}
label exit;
begin t_open_in;
if last>first then
  begin loc:=first;
  while (loc<last)and(buffer[loc]=" ") do incr(loc);
  if loc<last then
    begin init_terminal:=true; return;
    end;
  end;
@z

@x [4.49] l.1210 - printable characters are a table
@<Character |k| cannot be printed@>=
  (k<" ")or(k>"~")
@y
@<Character |k| cannot be printed@>=
  not printable[k]
@z

@x [4.51] l.1225 - the pool comes from the program itself
name_of_file:=pool_name; {we needn't set |name_length|}
if a_open_in(pool_file) then
  begin c:=false;
  repeat @<Read one string, but return |false| if the
    string memory space is getting too tight for comfort@>;
  until c;
  a_close(pool_file); get_strings_started:=true;
  end
else  bad_pool('! I can''t read TEX.POOL.')
@y
get_strings_started:=load_pool_strings
@z

@x [8.110] l.2406 - wider quarterwords and halfwords
@d min_quarterword=0 {smallest allowable value in a |quarterword|}
@d max_quarterword=255 {largest allowable value in a |quarterword|}
@d min_halfword==0 {smallest allowable value in a |halfword|}
@d max_halfword==65535 {largest allowable value in a |halfword|}
@y
@d min_quarterword=0 {smallest allowable value in a |quarterword|}
@d max_quarterword=@"FFFF {largest allowable value in a |quarterword|}
@d min_halfword==0 {smallest allowable value in a |halfword|}
@d max_halfword==@"FFFFFFF {largest allowable value in a |halfword|}
@z

@x [8.111] l.2428 - more than 256 fonts
if font_max>font_base+256 then bad:=16;
@y
@z

@x [12.186] l.3747 - glue ratios are doubles
  if abs(mem[p+glue_offset].int)<@'4000000 then print("?.?")
  else if abs(g)>float_constant(20000) then
@y
  if abs(g)>float_constant(20000) then
@z

@x [17.222] l.4515 - room for every font identifier
@d undefined_control_sequence=frozen_null_font+257 {dummy location}
@y
@d undefined_control_sequence=frozen_null_font+font_max+1 {dummy location}
@z

@x [17.241] l.5182 - the date and time
@p procedure fix_date_and_time;
begin sys_time:=12*60;
sys_day:=4; sys_month:=7; sys_year:=1776;  {self-evident truths}
@y
@p procedure fix_date_and_time;
begin @=system_date_and_time();@>
@z

@x [29.513] l.10069 - Unix file names
@!area_delimiter:pool_pointer; {the most recent `\.>' or `\.:', if any}
@!ext_delimiter:pool_pointer; {the relevant `\..', if any}
@y
@!area_delimiter:pool_pointer; {the most recent `\./', if any}
@!ext_delimiter:pool_pointer; {the most recent `\..' after it, if any}
@!quoted_filename:boolean; {inside a \.{"}...\.{"} part of the name?}
@z

@x [29.514] l.10083 - files are found by the resolver, not in fixed areas
@d TEX_area=="TeXinputs:"
@.TeXinputs@>
@d TEX_font_area=="TeXfonts:"
@.TeXfonts@>
@y
@z

@x [29.515] l.10091 - begin_name
begin area_delimiter:=0; ext_delimiter:=0;
@y
begin area_delimiter:=0; ext_delimiter:=0; quoted_filename:=false;
@z

@x [29.516] l.10102 - more_name: `/' ends the area, the last `.' starts the extension
begin if c=" " then more_name:=false
else  begin str_room(1); append_char(c); {contribute |c| to the current string}
  if (c=">")or(c=":") then
    begin area_delimiter:=cur_length; ext_delimiter:=0;
    end
  else if (c=".")and(ext_delimiter=0) then ext_delimiter:=cur_length;
  more_name:=true;
  end;
@y
begin if (c=" ")and not quoted_filename then more_name:=false
else if c="""" then
  begin quoted_filename:=not quoted_filename; more_name:=true;
  end
else  begin str_room(1); append_char(c); {contribute |c| to the current string}
  if c="/" then
    begin area_delimiter:=cur_length; ext_delimiter:=0;
    end
  else if c="." then ext_delimiter:=cur_length;
  more_name:=true;
  end;
@z

@x [29.520] l.10186 - the default format
@d format_default_length=20 {length of the |TEX_format_default| string}
@d format_area_length=11 {length of its area part}
@y
@d format_default_length=9 {length of the |TEX_format_default| string}
@d format_area_length=0 {length of its area part}
@z

@x [29.521] l.10195 - the default format
TEX_format_default:='TeXformats:plain.fmt';
@y
TEX_format_default:='plain.fmt';
@z

@x [29.524] - a format named by the caller (opt.format) comes first
begin j:=loc;
@y
begin j:=loc;
if open_option_fmt then goto found;
@z

@x [29.537] l.10354 - \input searches through the resolver
begin scan_file_name; {set |cur_name| to desired file name}
if cur_ext="" then cur_ext:=".tex";
pack_cur_name;
loop@+  begin begin_file_reading; {set up |cur_file| and new level of input}
  if a_open_in(cur_file) then goto done;
  if cur_area="" then
    begin pack_file_name(cur_name,TEX_area,cur_ext);
    if a_open_in(cur_file) then goto done;
    end;
  end_file_reading; {remove the level that didn't work}
@y
begin scan_file_name; {set |cur_name| to desired file name}
pack_cur_name; {the resolver tries the name with \.{.tex} first}
loop@+  begin begin_file_reading; {set up |cur_file| and new level of input}
  if a_open_in(cur_file,tex_kind) then goto done;
  end_file_reading; {remove the level that didn't work}
@z

@x [30.563] l.10913 - font metric files through the resolver
if aire="" then pack_file_name(nom,TEX_font_area,".tfm")
else pack_file_name(nom,aire,".tfm");
if not b_open_in(tfm_file) then abort;
@y
pack_file_name(nom,aire,".tfm");
if not b_open_in(tfm_file,tfm_kind) then abort;
@z

@x [49.1275] l.23376 - \openin searches through the resolver
  if cur_ext="" then cur_ext:=".tex";
  pack_cur_name;
  if a_open_in(read_file[n]) then read_open[n]:=just_open;
@y
  pack_cur_name;
  if a_open_in(read_file[n],tex_kind) then read_open[n]:=just_open;
@z

@x [51.1332] l.24277 - one program is both INITEX and the production version
@!init if not get_strings_started then goto final_end;
init_prim; {call |primitive| for each primitive}
init_str_ptr:=str_ptr; init_pool_ptr:=pool_ptr; fix_date_and_time;
tini@/
@y
if ini_version then
  begin if not get_strings_started then goto final_end;
  init_prim; {call |primitive| for each primitive}
  init_str_ptr:=str_ptr; init_pool_ptr:=pool_ptr; fix_date_and_time;
  end;
@z

@x [51.1337] - the caller's interaction mode (opt.interaction) overrides the format's
@<Initialize the print |selector|...@>;
if (loc<limit)and(cat_code(buffer[loc])<>escape) then start_input;
@y
apply_option_interaction;
@<Initialize the print |selector|...@>;
if (loc<limit)and(cat_code(buffer[loc])<>escape) then start_input;
@z

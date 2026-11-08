## The oracle behind tools/octave_parse_check.py and grammar_diff.py's
## matlab entry: for each file named in the list file argv(){1}, print one
## line -- "ok", or Octave's parse error as `__parse_file__` raises it
## (newlines folded, the " in file ..." tail kept for the caller to strip).
## Nothing in the files is run.
##
##   octave-cli --norc --quiet --no-gui tools/octave_parse_oracle.m LIST
args = argv ();
fid = fopen (args{1});
files = {};
while (true)
  l = fgetl (fid);
  if (! ischar (l))
    break;
  endif
  files{end+1} = l;
endwhile
fclose (fid);
warning ("off", "all");
for i = 1:numel (files)
  try
    __parse_file__ (files{i});
    printf ("ok\n");
  catch err
    printf ("%s\n", strrep (err.message, "\n", " "));
  end_try_catch
endfor

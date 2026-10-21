; Highlight captures for mep's Bash grammar (nvim-treesitter naming).
;
; Pattern order is load-bearing: two captures over the exact same bytes
; are resolved later-pattern-wins (see TreesitterHighlight's sort in
; treesitter.cpp), so the broad conventions come first and every narrower
; rule that should override them comes after.

; ---- Names ------------------------------------------------------------------

(variable_name) @variable

(special_variable_name) @variable.builtin

; The variables bash itself sets or reads (Bash manual, "Shell Variables").
((variable_name) @variable.builtin
 (#any-of? @variable.builtin
  "BASH" "BASHOPTS" "BASHPID" "BASH_ALIASES" "BASH_ARGC" "BASH_ARGV" "BASH_ARGV0"
  "BASH_CMDS" "BASH_COMMAND" "BASH_ENV" "BASH_LINENO" "BASH_REMATCH" "BASH_SOURCE"
  "BASH_SUBSHELL" "BASH_VERSINFO" "BASH_VERSION" "BASH_XTRACEFD" "CDPATH" "COLUMNS"
  "COMP_CWORD" "COMP_LINE" "COMP_POINT" "COMP_WORDS" "COMPREPLY" "DIRSTACK" "EPOCHREALTIME"
  "EPOCHSECONDS" "EUID" "FUNCNAME" "GLOBIGNORE" "GROUPS" "HISTFILE" "HISTSIZE" "HOME"
  "HOSTNAME" "HOSTTYPE" "IFS" "LANG" "LC_ALL" "LINENO" "LINES" "MACHTYPE" "OLDPWD" "OPTARG"
  "OPTERR" "OPTIND" "OSTYPE" "PATH" "PIPESTATUS" "PPID" "PS1" "PS2" "PS3" "PS4"
  "PROMPT_COMMAND" "PWD" "RANDOM" "READLINE_LINE" "REPLY" "SECONDS" "SHELL" "SHELLOPTS"
  "SHLVL" "SRANDOM" "TMOUT" "TMPDIR" "UID"))

; Positional parameters: $1, ${10}.
((variable_name) @variable.builtin
 (#match? @variable.builtin "^[0-9]+$"))

; Every command is a call; what it calls is decided below.
(command_name) @function.call

; Builtins (Bash manual, "Shell Builtin Commands").
(command_name
  (word) @function.builtin
  (#any-of? @function.builtin
   "." ":" "alias" "bg" "bind" "break" "builtin" "caller" "cd" "command" "compgen"
   "complete" "compopt" "continue" "dirs" "disown" "echo" "enable" "eval" "exec" "fc" "fg"
   "getopts" "hash" "help" "history" "jobs" "kill" "let" "logout" "mapfile" "popd" "printf"
   "pushd" "pwd" "read" "readarray" "set" "shift" "shopt" "source" "suspend" "test" "times"
   "trap" "type" "ulimit" "umask" "unalias" "wait"))

(command_name
  (word) @boolean
  (#any-of? @boolean "true" "false"))

(command_name
  (word) @keyword.return
  (#any-of? @keyword.return "return" "exit"))

(function_definition name: (word) @function)

; An option: `-x`, `--long`, `--key=value`.
(command
  argument: (word) @variable.parameter
  (#match? @variable.parameter "^--?[A-Za-z0-9]"))

; ---- Literals ---------------------------------------------------------------

[
  (string)
  (raw_string)
  (ansi_c_string)
  (translated_string)
  (heredoc_body)
] @string

[
  (heredoc_start)
  (heredoc_end)
] @label

[
  (regex)
  (extglob_pattern)
] @string.regexp

(number) @number

(file_descriptor) @number

(comment) @comment

; The interpreter line.
((program . (comment) @keyword.directive)
 (#match? @keyword.directive "^#!"))

; ---- Keywords ---------------------------------------------------------------

[
  "if"
  "then"
  "elif"
  "else"
  "fi"
  "case"
  "esac"
] @conditional

[
  "for"
  "select"
  "in"
  "while"
  "until"
  "do"
  "done"
] @repeat

"function" @keyword.function

[
  "declare"
  "typeset"
  "export"
  "readonly"
  "local"
  "unset"
  "unsetenv"
] @keyword

; ---- Operators and punctuation ----------------------------------------------

; Pipes, lists and redirections. The operators of expressions ([[ ]],
; (( )), ${...}) are their nodes' `operator` fields, below.
[
  "|"
  "|&"
  "&&"
  "||"
  ">"
  ">>"
  "<"
  "<<"
  "<<-"
  "<<<"
  "&>"
  "&>>"
  "<&"
  ">&"
  ">|"
  "<>"
  "<&-"
  ">&-"
] @operator

(negated_command "!" @operator)
(variable_assignment ["=" "+="] @operator)
(command ["=~" "=="] @operator)

; test_operator (`-f`, `-eq`) is always one of these fields.
(binary_expression operator: _ @operator)
(unary_expression operator: _ @operator)
(postfix_expression operator: _ @operator)
(expansion operator: _ @operator)

[
  ";"
  ";;"
  ";&"
  ";;&"
  "&"
] @punctuation.delimiter

[
  "("
  ")"
  "(("
  "))"
  "{"
  "}"
  "["
  "]"
  "[["
  "]]"
] @punctuation.bracket

; The delimiters of an expansion or substitution: `$`, `${ }`, `$( )`,
; `` ` ` ``, `$(( ))`, `<( )`.
(simple_expansion "$" @punctuation.special)
(expansion ["${" "}"] @punctuation.special)
(command_substitution ["$(" ")" "`"] @punctuation.special)
(process_substitution ["<(" ">(" ")"] @punctuation.special)
(arithmetic_expansion ["$((" "))"] @punctuation.special)

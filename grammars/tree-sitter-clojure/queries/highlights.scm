; Highlight captures for mep's Clojure grammar (nvim-treesitter naming).
;
; Pattern order is load-bearing: two captures over the exact same bytes
; are resolved later-pattern-wins (TreesitterHighlight's sort in
; treesitter.cpp), so the broad conventions come first and the narrower
; rules that override them after.
;
; The tree is the reader's: a `defn` is a list whose first symbol is
; `defn`. What a form means is decided here by that symbol's name, the
; same for core's macros as for a library's.

; Atoms

(sym_lit) @variable
(sym_ns) @module

(comment) @comment
; A form behind #_ is read and thrown away: shown as a comment.
(dis_expr) @comment

(str_lit) @string
(regex_lit) @string.regexp
(num_lit) @number
(char_lit) @character
(bool_lit) @boolean
(nil_lit) @constant.builtin

(kwd_lit) @constant
(kwd_ns) @module
(auto_res_mark) @constant

; ##Inf, ##-Inf, ##NaN
(sym_val_lit) @constant.builtin

; *dynamic-vars* by the earmuffs convention.
((sym_name) @variable.builtin
 (#match? @variable.builtin "^\\*.+\\*$"))

; %, %1, %& in #(...)
((sym_name) @variable.parameter
 (#match? @variable.parameter "^%[0-9&]*$"))

; Brackets and the reader macros

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
(set_lit "#" @punctuation.bracket)
(anon_fn_lit "#" @punctuation.special)

(quoting_lit "'" @punctuation.special)
(syn_quoting_lit "`" @punctuation.special)
(unquoting_lit "~" @punctuation.special)
(unquote_splicing_lit "~@" @punctuation.special)
(derefing_lit "@" @punctuation.special)
(var_quoting_lit "#'" @punctuation.special)
(meta_lit "^" @punctuation.special)
(old_meta_lit "#^" @punctuation.special)
(regex_lit "#" @punctuation.special)
(sym_val_lit "##" @punctuation.special)
(evaling_lit "#=" @keyword.directive)
(read_cond_lit "#?" @keyword.directive)
(splicing_read_cond_lit "#?@" @keyword.directive)
(ns_map_lit "#" @punctuation.special)

; #inst "...", #uuid "...", #js {...}, #user.Point{...}
(tagged_or_ctor_lit "#" @attribute)
(tagged_or_ctor_lit tag: (sym_lit) @attribute)

; ^:private, ^String: metadata reads as an annotation.
(meta_lit value: (kwd_lit) @attribute)
(meta_lit value: (sym_lit) @type)

; Calls: the first symbol of any list.

(list_lit . (sym_lit name: (sym_name) @function.call))

; #'name: a var.
(var_quoting_lit value: (sym_lit name: (sym_name) @function))

; Java interop: (.method obj), (Class. args), (Class/staticMethod ...)
((list_lit . (sym_lit name: (sym_name) @function.method.call))
 (#match? @function.method.call "^\\.[^.]"))
((list_lit . (sym_lit name: (sym_name) @constructor))
 (#match? @constructor "^[A-Z].*\\.$"))
((sym_lit namespace: (sym_ns) @type)
 (#match? @type "^[A-Z]"))

; clojure.core functions people call most -- not all 600: a long #any-of?
; list is compared on every list head, every pass.
((list_lit . (sym_lit name: (sym_name) @function.builtin))
 (#any-of? @function.builtin
  "str" "println" "print" "prn" "pr" "pr-str" "prn-str" "format" "printf"
  "map" "mapv" "filter" "filterv" "remove" "reduce" "reduce-kv" "keep" "mapcat"
  "into" "conj" "cons" "concat" "assoc" "assoc-in" "dissoc" "update" "update-in"
  "get" "get-in" "select-keys" "merge" "merge-with" "keys" "vals" "contains?"
  "first" "second" "rest" "next" "last" "butlast" "nth" "count" "empty?" "seq"
  "take" "drop" "take-while" "drop-while" "partition" "partition-by" "group-by"
  "frequencies" "sort" "sort-by" "reverse" "distinct" "range" "repeat" "iterate"
  "apply" "partial" "comp" "complement" "identity" "constantly" "juxt"
  "vec" "vector" "list" "hash-map" "hash-set" "set" "sorted-map" "sorted-set"
  "atom" "swap!" "reset!" "deref" "ref" "dosync" "alter" "agent" "send"
  "inc" "dec" "max" "min" "mod" "rem" "quot" "abs" "zero?" "pos?" "neg?"
  "even?" "odd?" "nil?" "some?" "some" "every?" "not-any?" "not-every?"
  "=" "not=" "==" "<" ">" "<=" ">=" "+" "-" "*" "/" "not"
  "string?" "keyword?" "symbol?" "map?" "vector?" "seq?" "fn?" "number?"
  "keyword" "symbol" "name" "namespace" "type" "class" "instance?"
  "ex-info" "ex-message" "ex-data" "throw" "slurp" "spit" "read-string"
  "meta" "with-meta" "vary-meta" "realized?" "force" "doall" "dorun"))

; The special forms and core's defining and control macros.
((list_lit . (sym_lit name: (sym_name) @keyword))
 (#any-of? @keyword
  "def" "do" "quote" "var" "fn" "fn*" "let" "let*" "letfn" "loop" "loop*"
  "recur" "set!" "new" "." ".." "monitor-enter" "monitor-exit"
  "defn" "defn-" "defmacro" "defmulti" "defmethod" "defonce" "defprotocol"
  "defrecord" "deftype" "definterface" "defstruct" "declare" "ns" "comment"
  "binding" "with-open" "with-redefs" "with-local-vars" "with-out-str" "locking"
  "delay" "future" "lazy-seq" "doto" "reify" "proxy" "extend-type"
  "extend-protocol" "extend" "gen-class" "import" "require" "use" "refer"
  "deftest" "testing" "is" "are"))

((list_lit . (sym_lit name: (sym_name) @conditional))
 (#any-of? @conditional
  "if" "if-not" "if-let" "if-some" "when" "when-not" "when-let" "when-some"
  "when-first" "cond" "condp" "cond->" "cond->>" "case" "and" "or"))

((list_lit . (sym_lit name: (sym_name) @repeat))
 (#any-of? @repeat "doseq" "dotimes" "for" "while"))

((list_lit . (sym_lit name: (sym_name) @exception))
 (#any-of? @exception "try" "catch" "finally" "throw"))

((list_lit . (sym_lit name: (sym_name) @operator))
 (#any-of? @operator "->" "->>" "as->" "some->" "some->>"))

; Definitions: the name after the defining form.

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @function)
  (#any-of? @_def "defn" "defn-" "defmulti" "defmethod" "deftest" "definline"))

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @function.macro)
  (#eq? @_def "defmacro"))

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @type)
  (#any-of? @_def "defrecord" "deftype" "defprotocol" "definterface" "defstruct"))

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @module)
  (#any-of? @_def "ns" "in-ns"))

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @variable)
  (#any-of? @_def "def" "defonce"))

; Parameters: the argument vector of defn/defmacro/fn (after the name and
; a docstring, if any), and each arity's vector, `([x] ...)`.

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit)
  . (vec_lit (sym_lit name: (sym_name) @variable.parameter))
  (#any-of? @_def "defn" "defn-" "defmacro" "fn" "fn*"))

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit)
  . (str_lit)
  . (vec_lit (sym_lit name: (sym_name) @variable.parameter))
  (#any-of? @_def "defn" "defn-" "defmacro"))

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (vec_lit (sym_lit name: (sym_name) @variable.parameter))
  (#any-of? @_def "fn" "fn*"))

(list_lit
  . (vec_lit (sym_lit name: (sym_name) @variable.parameter)))

; A docstring.
(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit)
  . (str_lit) @string.documentation
  . (_)
  (#any-of? @_def "defn" "defn-" "defmacro" "defmulti" "defprotocol" "ns"))

; `&` in an argument vector or a destructuring form: last, over the
; parameter rules above.
((sym_name) @operator
 (#eq? @operator "&"))

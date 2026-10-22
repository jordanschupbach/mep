; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
;
; Definitions are lists named by their first symbol, as in highlights.scm.
; Metadata on the name (`(defn ^:private f ...)`) is part of the name's
; sym_lit, so @name is the sym_name inside it.

(list_lit . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @name)
  (#any-of? @_def "defn" "defn-" "definline")) @definition.function

(list_lit . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @name)
  (#eq? @_def "defmacro")) @definition.macro

(list_lit . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @name)
  (#any-of? @_def "defmulti" "defmethod")) @definition.method

(list_lit . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @name)
  (#any-of? @_def "defrecord" "deftype" "defstruct")) @definition.struct

(list_lit . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @name)
  (#any-of? @_def "defprotocol" "definterface")) @definition.class

(list_lit . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @name)
  (#any-of? @_def "def" "defonce")) @definition.variable

(list_lit . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @name)
  (#eq? @_def "deftest")) @definition.function

(list_lit . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @name)
  (#eq? @_def "ns")) @definition.module

// The part of mep's Common Lisp vocabulary that carries prose: the
// special operators, the macros and functions common enough that an
// explanation in an editor popup earns its place, the lambda-list
// keywords, the declarations, and LOOP's clause keywords.
//
// Hand-written, unlike lisp_lsp_cl_names.cpp next to it, which is
// generated from SBCL and carries names, kinds, lambda lists and SBCL's
// own docstring summaries. The split is deliberate: the generated half
// stays mechanically true to the image it came from, and many standard
// operators have no docstring there at all (SBCL documents `defun` in
// one line and `loop` not at all). What is written here says the thing
// a person editing a `.lisp` file actually needs, which is often "this
// is the one that does not evaluate its argument" or "this is the
// trap".

#include <string>
#include <vector>

#include "lisp_lsp.h"

namespace {

const std::vector<LispLspVocabEntry> kSpecialForms = {
    {"BLOCK", "(block name form*)",
     "Establishes a block named `name` that `return-from name` can exit early with a value. `defun` wraps its body "
     "in a block named after the function."},
    {"CATCH", "(catch tag form*)",
     "Evaluates the forms; a `throw` to an `eq` tag anywhere in their dynamic extent unwinds to here. Unlike "
     "`block`, the tag is an evaluated object, not a lexical name."},
    {"EVAL-WHEN", "(eval-when (situation*) form*)",
     "Controls when the forms are evaluated: `:compile-toplevel` (while compiling the file), `:load-toplevel` (when "
     "the compiled file is loaded) and `:execute` (when evaluated or loaded as source). Macros' helper functions "
     "usually need all three."},
    {"FLET", "(flet ((name lambda-list form*)*) form*)",
     "Defines local functions visible in the body only. The functions cannot call themselves or each other; use "
     "`labels` for that."},
    {"FUNCTION", "(function name) / #'name",
     "Returns the function a name denotes in the function namespace, or a closure for a `lambda` expression. `#'foo` "
     "is read as `(function foo)`."},
    {"GO", "(go tag)", "Transfers control to `tag` in an enclosing `tagbody`. Tags are lexical: a `go` sees only the "
                       "tagbodies around it in the source."},
    {"IF", "(if test then [else])",
     "Evaluates `then` when `test` is true (anything but `nil`), otherwise `else`, which defaults to `nil`. Takes "
     "exactly one form per branch: wrap several in `progn`, or use `when`/`cond`."},
    {"LABELS", "(labels ((name lambda-list form*)*) form*)",
     "Like `flet`, but each local function is visible inside every definition too, so they can recurse and call "
     "each other."},
    {"LET", "(let ((var [init])*) declaration* form*)",
     "Binds variables in parallel: every init form is evaluated before any variable is bound, so no init sees "
     "another one's variable. Use `let*` for sequential binding."},
    {"LET*", "(let* ((var [init])*) declaration* form*)",
     "Binds variables one after another, so each init form sees the variables bound before it."},
    {"LOAD-TIME-VALUE", "(load-time-value form [read-only-p])",
     "Evaluates `form` once, when the compiled file is loaded, and uses that value every time the surrounding code "
     "runs. The form sees no lexical variables."},
    {"LOCALLY", "(locally declaration* form*)",
     "Evaluates the forms with the given declarations in effect, without binding anything."},
    {"MACROLET", "(macrolet ((name lambda-list form*)*) form*)",
     "Defines local macros for the body. The macro bodies run at compile time and cannot see the body's lexical "
     "variables."},
    {"MULTIPLE-VALUE-CALL", "(multiple-value-call function form*)",
     "Calls `function` with *all* the values of every form as its arguments, rather than only the first value of "
     "each."},
    {"MULTIPLE-VALUE-PROG1", "(multiple-value-prog1 first-form form*)",
     "Like `prog1`, but keeps every value of the first form, not only the primary one."},
    {"PROGN", "(progn form*)", "Evaluates the forms in order and returns the values of the last one."},
    {"PROGV", "(progv symbols values form*)",
     "Binds dynamic variables whose names are computed at run time. Rarely needed: `let` on a `defvar`'d variable "
     "is the normal way to rebind a special."},
    {"QUOTE", "(quote object) / 'object",
     "Returns its argument unevaluated. Literal data is shared and must not be modified: `(setf (car '(1 2)) 3)` "
     "has undefined consequences."},
    {"RETURN-FROM", "(return-from name [result])",
     "Exits the lexically enclosing block named `name`, returning `result`. `return` is shorthand for a block "
     "named `nil`, which every loop establishes."},
    {"SETQ", "(setq var form {var form}*)",
     "Assigns to variables, pairwise and in order. `setf` does the same for a plain variable and also accepts "
     "places like `(car x)` or `(gethash k table)`."},
    {"SYMBOL-MACROLET", "(symbol-macrolet ((symbol expansion)*) form*)",
     "Makes each symbol stand for an expansion form within the body, so reading or `setf`ing the symbol reads or "
     "assigns the form. `with-slots` is built on this."},
    {"TAGBODY", "(tagbody {tag | statement}*)",
     "Evaluates the statements in order and returns `nil`; symbols and integers in the body are tags `go` can jump "
     "to. Every `do` and `dolist` body is an implicit tagbody."},
    {"THE", "(the type form)",
     "Promises that `form` returns a value of `type`. A safe compiler checks the promise; an unsafe one trusts it, "
     "so a false `the` is undefined behavior, not documentation."},
    {"THROW", "(throw tag result)", "Unwinds to the most recent dynamically enclosing `catch` for an `eq` tag, "
                                    "making `result` the value of that catch."},
    {"UNWIND-PROTECT", "(unwind-protect protected-form cleanup-form*)",
     "Evaluates the protected form and then always the cleanup forms, however control leaves -- a normal return, "
     "a `throw`, a `return-from` or an unhandled error."},
};

const std::vector<LispLspVocabEntry> kMacros = {
    {"DEFUN", "(defun name lambda-list [docstring] declaration* form*)",
     "Defines a global function. The body is wrapped in a block named after the function, so `return-from name` "
     "exits it."},
    {"DEFMACRO", "(defmacro name lambda-list [docstring] declaration* form*)",
     "Defines a macro: a function from the call's unevaluated forms to the code that replaces it. Use `&body` for "
     "the body parameter so editors indent calls properly."},
    {"DEFGENERIC", "(defgeneric name lambda-list option*)",
     "Declares a generic function and its lambda list, which every method must be congruent with. Methods are "
     "added with `defmethod` or `(:method ...)` options."},
    {"DEFMETHOD", "(defmethod name qualifier* specialized-lambda-list form*)",
     "Adds a method to a generic function, creating the generic function if there is none. Required parameters may "
     "be specialized as `(param class)` or `(param (eql value))`; `:before`, `:after` and `:around` are the "
     "standard qualifiers."},
    {"DEFVAR", "(defvar name [value [docstring]])",
     "Declares a special (dynamically scoped) variable. The value is only assigned if the variable is unbound, so "
     "reloading the file keeps the current value. Name it with *earmuffs*."},
    {"DEFPARAMETER", "(defparameter name value [docstring])",
     "Declares a special variable and always assigns it, every time the form is evaluated -- the difference from "
     "`defvar`. Name it with *earmuffs*."},
    {"DEFCONSTANT", "(defconstant name value [docstring])",
     "Declares a named constant. Redefining it with a value that is not `eql` is undefined, which bites with lists "
     "and strings: use `defparameter` (or alexandria's `define-constant`) for those. Conventionally +plus+ named."},
    {"DEFCLASS", "(defclass name (superclass*) (slot-spec*) option*)",
     "Defines a CLOS class. Each slot takes options such as `:initarg`, `:initform`, `:accessor`, `:reader`, "
     "`:type` and `:documentation`."},
    {"DEFSTRUCT", "(defstruct name-and-options [docstring] slot-description*)",
     "Defines a structure type with a constructor `make-NAME`, a predicate `NAME-p`, a copier `copy-NAME` and an "
     "accessor `NAME-SLOT` per slot. Redefining a structure incompatibly is undefined, unlike a class."},
    {"DEFINE-CONDITION", "(define-condition name (parent*) (slot-spec*) option*)",
     "Defines a condition type for `signal`, `error` and `handler-case`. `(:report ...)` gives it a printed "
     "message, either a string or a function of the condition and a stream."},
    {"DEFTYPE", "(deftype name lambda-list form*)",
     "Defines a type specifier by expansion into other type specifiers, like a macro for types."},
    {"DEFPACKAGE", "(defpackage name option*)",
     "Defines a package: which packages it `:use`s, what it `:export`s, imports and shadows. Write symbol names "
     "as `#:name` or strings so reading the form does not intern them in the current package."},
    {"IN-PACKAGE", "(in-package name)",
     "Makes the named package current for the rest of the file: every symbol read after it is interned there."},
    {"DEFINE-COMPILER-MACRO", "(define-compiler-macro name lambda-list form*)",
     "Defines an optional, compile-time rewrite of calls to a function. Returning the `&whole` form unchanged "
     "declines the rewrite."},
    {"DEFSETF", "(defsetf access-fn update-fn) / (defsetf access-fn lambda-list (store-var) form*)",
     "Teaches `setf` how to assign through a call to `access-fn`."},
    {"DEFINE-MODIFY-MACRO", "(define-modify-macro name lambda-list function [docstring])",
     "Defines a read-modify-write macro like `incf`: `(name place args...)` sets the place to `(function place "
     "args...)`."},
    {"DEFINE-SYMBOL-MACRO", "(define-symbol-macro symbol expansion)",
     "Makes a global symbol stand for an expansion form, as `symbol-macrolet` does locally."},
    {"LAMBDA", "(lambda lambda-list form*)", "An anonymous function. As a macro it expands to `#'(lambda ...)`."},
    {"WHEN", "(when test form*)", "Evaluates the forms when `test` is true and returns the last value; otherwise "
                                  "returns `nil`. Prefer it to an `if` without an else branch."},
    {"UNLESS", "(unless test form*)", "Evaluates the forms when `test` is false; otherwise returns `nil`."},
    {"COND", "(cond (test form*)*)",
     "Tries each clause's test in order and evaluates the forms of the first one that is true. A clause with no "
     "forms returns the test's value; a final `(t ...)` clause is the default."},
    {"CASE", "(case keyform (keys form*)* [(otherwise form*)])",
     "Compares the key with `eql` against each clause's keys, which are *not evaluated*: write `(:a ...)` or "
     "`((1 2) ...)`, never `('a ...)` (that matches the symbols `quote` and `a`). `t` or `otherwise` is the default."},
    {"ECASE", "(ecase keyform (keys form*)*)", "Like `case`, but signals an error when no clause matches."},
    {"TYPECASE", "(typecase keyform (type form*)*)", "Picks the first clause whose type the key's value is of."},
    {"ETYPECASE", "(etypecase keyform (type form*)*)", "Like `typecase`, but signals an error when no type matches."},
    {"AND", "(and form*)", "Evaluates forms left to right, stopping at the first `nil`; returns the last value."},
    {"OR", "(or form*)", "Evaluates forms left to right and returns the first true value, or `nil`."},
    {"PROG1", "(prog1 first-form form*)", "Evaluates every form and returns the primary value of the first."},
    {"PROG2", "(prog2 first-form second-form form*)", "Evaluates every form and returns the value of the second."},
    {"SETF", "(setf place value {place value}*)",
     "Stores values into places: variables, `(car x)`, `(aref a i)`, `(gethash k h)`, `(slot-value o 's)`, an "
     "accessor, or anything with a `defsetf`/`(setf name)` function."},
    {"PSETF", "(psetf place value {place value}*)", "Like `setf`, but computes every value before storing any."},
    {"PSETQ", "(psetq var form {var form}*)", "Like `setq`, but computes every value before assigning any."},
    {"INCF", "(incf place [delta])", "Adds `delta` (default 1) to the number in `place` and stores the result."},
    {"DECF", "(decf place [delta])", "Subtracts `delta` (default 1) from the number in `place`."},
    {"PUSH", "(push item place)", "Conses `item` onto the list in `place` and stores the new list."},
    {"POP", "(pop place)", "Returns the first element of the list in `place` and stores the rest back."},
    {"PUSHNEW", "(pushnew item place &key key test test-not)",
     "Like `push`, unless the item is already in the list (by `eql`, or `:test`)."},
    {"DOLIST", "(dolist (var list [result]) declaration* {tag | form}*)",
     "Evaluates the body once per element of `list`, with `var` bound to the element, and returns `result`."},
    {"DOTIMES", "(dotimes (var count [result]) declaration* {tag | form}*)",
     "Evaluates the body with `var` bound to 0, 1, ... below `count`, and returns `result`."},
    {"DO", "(do ((var [init [step]])*) (end-test result*) declaration* {tag | form}*)",
     "General iteration with parallel stepping: all steps are computed before any variable is updated. The loop "
     "ends when `end-test` is true and returns the result forms' values."},
    {"DO*", "(do* ((var [init [step]])*) (end-test result*) declaration* {tag | form}*)",
     "Like `do`, but binds and steps the variables sequentially."},
    {"LOOP", "(loop clause*)",
     "The iteration language: `for`/`as` drive variables, `collect`/`sum`/`count`/... accumulate, `while`/"
     "`until`/`when` control, `do` runs forms, `finally` and `return` finish. A loop of plain forms repeats them "
     "forever."},
    {"RETURN", "(return [result])", "Returns from the nearest enclosing block named `nil` -- every `loop`, `do`, "
                                    "`dolist` and `dotimes` establishes one."},
    {"DESTRUCTURING-BIND", "(destructuring-bind lambda-list expression declaration* form*)",
     "Binds the variables of a macro-style lambda list to the parts of a list, signalling an error when the shape "
     "does not match."},
    {"MULTIPLE-VALUE-BIND", "(multiple-value-bind (var*) values-form declaration* form*)",
     "Binds variables to the values a form returns; missing values bind to `nil`, extra ones are dropped."},
    {"MULTIPLE-VALUE-SETQ", "(multiple-value-setq (var*) form)", "Assigns the values of a form to existing variables."},
    {"MULTIPLE-VALUE-LIST", "(multiple-value-list form)", "Returns all the values of a form as a list."},
    {"NTH-VALUE", "(nth-value n form)", "Returns the n-th (0-based) value of a form."},
    {"HANDLER-CASE", "(handler-case form (type ([var]) form*)*)",
     "Evaluates `form`; if a condition of a clause's type is signalled, control unwinds out of the form and that "
     "clause runs, with `var` bound to the condition. `(:no-error (values...) ...)` runs on normal completion."},
    {"HANDLER-BIND", "(handler-bind ((type handler)*) form*)",
     "Runs a handler function *without unwinding* when a matching condition is signalled; the handler may decline "
     "by returning, or transfer control (often by invoking a restart)."},
    {"IGNORE-ERRORS", "(ignore-errors form*)",
     "Evaluates the forms; on an `error` condition returns `nil` and the condition instead of entering the debugger."},
    {"RESTART-CASE", "(restart-case form (name lambda-list option* form*)*)",
     "Evaluates `form` with named restarts available, which a handler or the debugger can invoke to recover."},
    {"WITH-SIMPLE-RESTART", "(with-simple-restart (name format-control arg*) form*)",
     "Offers one restart that, when invoked, returns `nil` and `t` from the form."},
    {"WITH-OPEN-FILE", "(with-open-file (stream filespec option*) declaration* form*)",
     "Opens a file for the body and always closes it afterwards, however the body exits. Options are those of "
     "`open`: `:direction`, `:if-exists`, `:if-does-not-exist`, `:element-type`, `:external-format`."},
    {"WITH-OPEN-STREAM", "(with-open-stream (var stream) form*)", "Binds a stream and closes it when the body exits."},
    {"WITH-OUTPUT-TO-STRING", "(with-output-to-string (var [string]) form*)",
     "Binds `var` to a string output stream and returns everything written to it."},
    {"WITH-INPUT-FROM-STRING", "(with-input-from-string (var string &key start end index) form*)",
     "Binds `var` to an input stream reading from a string."},
    {"WITH-SLOTS", "(with-slots (slot-entry*) instance form*)",
     "Makes slot names (or `(var slot)` pairs) stand for `(slot-value instance 'slot)` in the body, readable and "
     "setf-able."},
    {"WITH-ACCESSORS", "(with-accessors ((var accessor)*) instance form*)",
     "Makes each variable stand for a call of an accessor on the instance, readable and setf-able."},
    {"WITH-STANDARD-IO-SYNTAX", "(with-standard-io-syntax form*)",
     "Binds every printer and reader variable to its standard value, so data printed in the body reads back."},
    {"PRINT-UNREADABLE-OBJECT", "(print-unreadable-object (object stream &key type identity) form*)",
     "Prints `#<...>` around whatever the body writes; the usual body of a `print-object` method."},
    {"ASSERT", "(assert test [(place*) [datum arg*]])",
     "Signals a correctable error when `test` is false; the continue restart lets the user supply new values for "
     "the places and retries."},
    {"CHECK-TYPE", "(check-type place type [type-string])",
     "Signals a correctable error unless the place's value is of `type`, which is not evaluated."},
    {"DECLAIM", "(declaim declaration-specifier*)",
     "Makes global declarations at compile time -- `(declaim (optimize speed))`, `(declaim (inline f))`, "
     "`(declaim (ftype (function (fixnum) fixnum) f))`."},
    {"TIME", "(time form)", "Evaluates the form and prints how long it took and how much it allocated."},
    {"STEP", "(step form)", "Evaluates the form in the implementation's single-stepper."},
    {"TRACE", "(trace function-name*)", "Prints each call to and return from the named functions."},
    {"FORMATTER", "(formatter control-string)", "Compiles a format control string into a function."},
    {"CALL-METHOD", "(call-method method [next-methods])", "Used inside `define-method-combination` only."},
    {"DEFINE-METHOD-COMBINATION", "(define-method-combination name ...)",
     "Defines how a generic function combines its applicable methods."},
    {"WITH-HASH-TABLE-ITERATOR", "(with-hash-table-iterator (name table) form*)",
     "Defines a local macro `(name)` that returns the next entry of the table on each call."},
    {"DO-SYMBOLS", "(do-symbols (var [package [result]]) form*)", "Iterates over every symbol accessible in a package."},
    {"DO-EXTERNAL-SYMBOLS", "(do-external-symbols (var [package [result]]) form*)",
     "Iterates over the external symbols of a package."},
    {"DO-ALL-SYMBOLS", "(do-all-symbols (var [result]) form*)", "Iterates over every symbol in every package."},
};

const std::vector<LispLspVocabEntry> kFunctions = {
    {"CAR", "(car list)", "The first element of a list (`nil` for `nil`). Also a place for `setf`."},
    {"CDR", "(cdr list)", "Everything after the first element of a list (`nil` for `nil`)."},
    {"CONS", "(cons a b)", "Makes a fresh pair whose car is `a` and cdr is `b`; a list is a chain of them."},
    {"LIST", "(list &rest objects)", "Returns a fresh list of its arguments."},
    {"APPEND", "(append &rest lists)", "Concatenates lists, copying all but the last, which is shared."},
    {"NCONC", "(nconc &rest lists)", "Destructively concatenates lists. Never call it on literal (quoted) data."},
    {"MAPCAR", "(mapcar function list &rest more-lists)",
     "Calls the function on successive elements and returns the list of results, stopping at the shortest list."},
    {"MAPC", "(mapc function list &rest more-lists)", "Like `mapcar` for side effects: returns the first list."},
    {"MAPCAN", "(mapcan function list &rest more-lists)", "Like `mapcar`, but `nconc`s the results together."},
    {"REDUCE", "(reduce function sequence &key key from-end start end initial-value)",
     "Combines a sequence's elements pairwise with a two-argument function."},
    {"FUNCALL", "(funcall function &rest args)", "Calls a function object (or a symbol's global function)."},
    {"APPLY", "(apply function &rest args+list)", "Calls a function with the arguments given, the last of which is "
                                                  "a list spread into the call."},
    {"EQ", "(eq a b)",
     "True when the two are the same object. Unreliable for numbers and characters -- use `eql` for those, which "
     "is what every test defaults to."},
    {"EQL", "(eql a b)", "Like `eq`, but also true for equal numbers of the same type and for equal characters."},
    {"EQUAL", "(equal a b)", "Structural equality for conses, strings and bit vectors; `eql` for everything else. "
                             "Case-sensitive on strings."},
    {"EQUALP", "(equalp a b)",
     "Looser structural equality: case-insensitive on strings and characters, numbers by `=`, and it descends into "
     "arrays, structures and hash tables."},
    {"GETHASH", "(gethash key table &optional default)",
     "Returns the value stored under a key and a second value telling whether it was there. `(setf gethash)` "
     "stores one."},
    {"MAKE-HASH-TABLE", "(make-hash-table &key test size rehash-size rehash-threshold)",
     "Makes a hash table. The default `:test` is `eql`, so string keys need `:test 'equal`."},
    {"REMHASH", "(remhash key table)", "Removes an entry; returns true if there was one."},
    {"MAPHASH", "(maphash function table)", "Calls a two-argument function on every key and value of a table."},
    {"FORMAT", "(format destination control-string &rest args)",
     "Formatted output: destination `t` is standard output, `nil` returns a string, a stream writes to it. "
     "Directives start with `~`: `~a` aesthetic, `~s` readable, `~d` decimal, `~%` newline, `~{~}` iteration."},
    {"PRINT", "(print object &optional stream)", "Prints a newline, then the object readably, then a space."},
    {"PRINC", "(princ object &optional stream)", "Prints an object for people: strings without quotes."},
    {"PRIN1", "(prin1 object &optional stream)", "Prints an object readably, with no surrounding whitespace."},
    {"TERPRI", "(terpri &optional stream)", "Writes a newline."},
    {"READ", "(read &optional stream eof-error-p eof-value recursive-p)",
     "Reads one Lisp object. With `*read-eval*` true, `#.` in the input runs code: bind it to nil for untrusted "
     "data."},
    {"READ-LINE", "(read-line &optional stream eof-error-p eof-value recursive-p)",
     "Reads a line as a string; the second value is true if it ended at end of file rather than a newline."},
    {"ERROR", "(error datum &rest arguments)",
     "Signals an error: `datum` is a format string, a condition type, or a condition. Never returns normally."},
    {"SIGNAL", "(signal datum &rest arguments)", "Signals a condition; returns `nil` if no handler takes over."},
    {"WARN", "(warn datum &rest arguments)", "Signals a warning and prints it if no handler takes over."},
    {"MAKE-INSTANCE", "(make-instance class &rest initargs)", "Creates an instance of a CLOS class."},
    {"SLOT-VALUE", "(slot-value object slot-name)", "Reads a slot by name; setf-able."},
    {"CALL-NEXT-METHOD", "(call-next-method &rest args)",
     "Inside a method, calls the next most specific method -- with the same arguments when given none."},
    {"NOT", "(not x)", "True when `x` is `nil`. Identical to `null`; by convention `not` is for booleans and `null` "
                       "for lists."},
    {"NULL", "(null x)", "True when `x` is `nil` (the empty list)."},
    {"LENGTH", "(length sequence)", "The number of elements in a list or vector. Walks the whole list each time."},
    {"NTH", "(nth n list)", "The n-th (0-based) element of a list. Note the argument order: index first."},
    {"ELT", "(elt sequence index)", "The element at an index of any sequence. Note the argument order: sequence first."},
    {"AREF", "(aref array &rest subscripts)", "The array element at the given subscripts; setf-able."},
    {"SUBSEQ", "(subseq sequence start &optional end)", "A fresh copy of part of a sequence."},
    {"FIND", "(find item sequence &key test key start end from-end)", "The first matching element, or nil."},
    {"POSITION", "(position item sequence &key test key start end from-end)", "The index of the first match, or nil."},
    {"MEMBER", "(member item list &key test key)", "The tail of the list starting at the first match, or nil."},
    {"ASSOC", "(assoc item alist &key test key)", "The first `(key . value)` pair whose key matches, or nil."},
    {"REMOVE", "(remove item sequence &key test key start end count from-end)",
     "A copy of a sequence without the matching elements. `delete` is the destructive version."},
    {"SORT", "(sort sequence predicate &key key)",
     "Destructively sorts a sequence and returns the result -- which, for a list, may not start at the old first "
     "cons. Always use the return value; copy literal data first."},
    {"STRING=", "(string= a b &key start1 end1 start2 end2)", "Case-sensitive string comparison."},
    {"STRING-EQUAL", "(string-equal a b &key start1 end1 start2 end2)", "Case-insensitive string comparison."},
    {"CONCATENATE", "(concatenate result-type &rest sequences)", "Joins sequences into a new one of the given type, "
                                                                 "e.g. `(concatenate 'string a b)`."},
    {"COERCE", "(coerce object result-type)", "Converts a sequence or number to another type."},
    {"TYPEP", "(typep object type &optional environment)", "True when the object is of the type (which is evaluated "
                                                           "-- usually quoted)."},
    {"VALUES", "(values &rest objects)", "Returns several values. `(values)` returns none."},
    {"IDENTITY", "(identity object)", "Returns its argument."},
    {"GENSYM", "(gensym &optional prefix)", "A fresh uninterned symbol, for macro-generated variables."},
    {"INTERN", "(intern string &optional package)", "Finds or creates the symbol with exactly that (case-sensitive) "
                                                    "name in a package."},
    {"SYMBOL-NAME", "(symbol-name symbol)", "The symbol's name string, upper-case for symbols read normally."},
    {"FDEFINITION", "(fdefinition name)", "The global function for a name, including `(setf foo)` names; setf-able."},
    {"SYMBOL-FUNCTION", "(symbol-function symbol)", "The global function of a symbol; setf-able."},
    {"EVAL", "(eval form)", "Evaluates a form in the null lexical environment: it cannot see local variables."},
    {"COMPILE", "(compile name &optional definition)", "Compiles a function at run time."},
    {"LOAD", "(load filespec &key verbose print if-does-not-exist external-format)",
     "Loads a source or compiled file, evaluating its top-level forms."},
    {"REQUIRE", "(require module-name &optional pathnames)", "Loads a module, implementation-defined (SBCL: contribs)."},
    {"1+", "(1+ number)", "The number plus one."},
    {"1-", "(1- number)", "The number minus one."},
    {"FLOOR", "(floor number &optional divisor)", "Truncates toward negative infinity; the second value is the "
                                                  "remainder."},
    {"MOD", "(mod number divisor)", "The remainder of `floor`, with the divisor's sign."},
    {"REM", "(rem number divisor)", "The remainder of `truncate`, with the dividend's sign."},
};

const std::vector<LispLspVocabEntry> kLambdaKeywords = {
    {"&OPTIONAL", "&optional {var | (var [default [supplied-p]])}*",
     "The parameters after this may be omitted by the caller and take their default (or `nil`). Mixing it with "
     "`&key` is legal but a trap: an omitted optional swallows the first keyword."},
    {"&REST", "&rest var", "Binds one variable to the list of all remaining arguments."},
    {"&BODY", "&body var", "Like `&rest`, in a macro lambda list, and tells editors the rest is a body to indent."},
    {"&KEY", "&key {var | ({var | (keyword var)} [default [supplied-p]])}*",
     "Keyword parameters, passed as `:name value` pairs in any order. Unknown keywords are an error unless "
     "`&allow-other-keys` is given or the call passes `:allow-other-keys t`."},
    {"&ALLOW-OTHER-KEYS", "&allow-other-keys", "Accepts keyword arguments the lambda list does not name."},
    {"&AUX", "&aux {var | (var [init])}*", "Not parameters at all: local variables bound like `let*` after the real "
                                           "parameters."},
    {"&WHOLE", "&whole var", "In a macro lambda list, binds the entire macro call form."},
    {"&ENVIRONMENT", "&environment var",
     "In a macro lambda list, binds the lexical environment, to pass on to `macroexpand` and friends."},
};

const std::vector<LispLspVocabEntry> kDeclarations = {
    {"DECLARE", "(declare declaration-specifier*)",
     "Declarations for the enclosing binding form; only allowed at the start of a body, before any other form."},
    {"IGNORE", "(declare (ignore var*))", "Says a variable is deliberately unused. Using it anyway is a warning."},
    {"IGNORABLE", "(declare (ignorable var*))", "Says a variable may or may not be used -- typical in macro output."},
    {"SPECIAL", "(declare (special var*))", "Makes a binding (or a reference) dynamic rather than lexical."},
    {"TYPE", "(declare (type type var*))", "Promises the variables hold values of the type."},
    {"FTYPE", "(declaim (ftype (function (arg-type*) result-type) name*))", "Declares a function's signature."},
    {"INLINE", "(declaim (inline name*))", "Allows the compiler to open-code calls to the functions; must precede "
                                           "their definition to take effect."},
    {"NOTINLINE", "(declare (notinline name*))", "Forbids open-coding, so redefinitions are always seen."},
    {"OPTIMIZE", "(declare (optimize (quality value)*))",
     "Sets compiler priorities from 0 to 3: `speed`, `safety`, `debug`, `space`, `compilation-speed`. "
     "`(safety 0)` removes checks entirely."},
    {"DYNAMIC-EXTENT", "(declare (dynamic-extent var*))",
     "Promises a value does not outlive the binding, letting it be stack-allocated."},
};

const std::vector<LispLspVocabEntry> kLoopKeywords = {
    {"FOR", "for var {in|on|across|from|=|being} ...",
     "Introduces an iteration variable. `in list` steps elements, `on list` steps tails, `across vector` steps "
     "vector elements, `from a to b by c` counts, `= init then step` computes."},
    {"AS", "as var ...", "A synonym for `for`."},
    {"WITH", "with var [= value]", "Binds a variable once, before the loop starts; `and` binds in parallel."},
    {"IN", "for x in list [by step-fn]", "Steps through the elements of a list."},
    {"ON", "for tail on list [by step-fn]", "Steps through the successive tails of a list."},
    {"ACROSS", "for x across vector", "Steps through the elements of a vector or string."},
    {"FROM", "for i from start", "The first value of a counting variable (0 when omitted)."},
    {"UPFROM", "for i upfrom start", "Counts up from a start value."},
    {"DOWNFROM", "for i downfrom start", "Counts down from a start value."},
    {"TO", "for i from a to b", "The inclusive limit of a counting variable."},
    {"UPTO", "for i upto b", "The inclusive upper limit."},
    {"BELOW", "for i below n", "The exclusive upper limit: the loop stops before reaching it."},
    {"DOWNTO", "for i downto b", "The inclusive lower limit when counting down."},
    {"ABOVE", "for i above n", "The exclusive lower limit when counting down."},
    {"BY", "for i from a by step", "The increment of a counting variable, or the step function for `in`/`on`."},
    {"THEN", "for x = init then step", "The form computing the variable's value on every iteration after the first."},
    {"BEING", "for k being the hash-keys of table [using (hash-value v)]",
     "Iterates over a hash table's keys or values, or a package's symbols."},
    {"USING", "using (hash-value v)", "Names the other half of a hash-table entry while iterating its keys or values."},
    {"REPEAT", "repeat n", "Runs the loop body n times."},
    {"WHILE", "while test", "Ends the loop (running `finally`) as soon as the test is false."},
    {"UNTIL", "until test", "Ends the loop (running `finally`) as soon as the test is true."},
    {"ALWAYS", "always test", "Returns nil as soon as the test is false; t if the loop finishes."},
    {"NEVER", "never test", "Returns nil as soon as the test is true; t if the loop finishes."},
    {"THEREIS", "thereis test", "Returns the test's value as soon as it is true; nil if the loop finishes."},
    {"DO", "do form+", "Evaluates forms on every iteration."},
    {"DOING", "doing form+", "A synonym for `do`."},
    {"COLLECT", "collect form [into var]", "Accumulates the values into a list, returned at the end unless `into` "
                                           "names a variable."},
    {"COLLECTING", "collecting form [into var]", "A synonym for `collect`."},
    {"APPEND", "append form [into var]", "Accumulates by appending lists."},
    {"NCONC", "nconc form [into var]", "Accumulates by destructively concatenating lists."},
    {"COUNT", "count form [into var]", "Counts the iterations on which the form is true."},
    {"SUM", "sum form [into var]", "Adds up the values."},
    {"SUMMING", "summing form [into var]", "A synonym for `sum`."},
    {"MAXIMIZE", "maximize form [into var]", "Keeps the largest value."},
    {"MINIMIZE", "minimize form [into var]", "Keeps the smallest value."},
    {"INTO", "collect x into var", "Accumulates into a named variable instead of the loop's return value; return it "
                                   "yourself, usually from `finally`."},
    {"WHEN", "when test clause", "Runs the following clause only on iterations where the test is true; `it` refers "
                                 "to the test's value."},
    {"IF", "if test clause", "A synonym for `when`."},
    {"UNLESS", "unless test clause", "Runs the following clause only when the test is false."},
    {"ELSE", "when test clause else clause", "The alternative clause of a `when`/`if`/`unless`."},
    {"END", "when test clause end", "Ends a conditional clause explicitly."},
    {"RETURN", "return form", "Returns the form's value from the loop immediately, skipping `finally`."},
    {"INITIALLY", "initially form+", "Forms run once before the first iteration."},
    {"FINALLY", "finally form+", "Forms run once after the last iteration -- but not after `return`."},
    {"NAMED", "loop named name ...", "Names the loop's block, so `return-from name` can exit it."},
    {"OF-TYPE", "for x of-type fixnum ...", "Declares the type of a loop variable."},
};

/** @brief Finds an entry by exact canonical name. */
const LispLspVocabEntry *FindIn(const std::vector<LispLspVocabEntry> &table, const std::string &name) {
    for (const LispLspVocabEntry &e : table) {
        if (name == e.name) return &e;
    }
    return nullptr;
}

}  // namespace

const std::vector<LispLspVocabEntry> &LispLspSpecialFormVocab() { return kSpecialForms; }
const std::vector<LispLspVocabEntry> &LispLspMacroVocab() { return kMacros; }
const std::vector<LispLspVocabEntry> &LispLspFunctionVocab() { return kFunctions; }
const std::vector<LispLspVocabEntry> &LispLspLambdaKeywordVocab() { return kLambdaKeywords; }
const std::vector<LispLspVocabEntry> &LispLspDeclarationVocab() { return kDeclarations; }
const std::vector<LispLspVocabEntry> &LispLspLoopKeywordVocab() { return kLoopKeywords; }

const LispLspVocabEntry *LispLspFindVocab(const std::string &upper_name) {
    for (const std::vector<LispLspVocabEntry> *table :
         {&kSpecialForms, &kMacros, &kFunctions, &kLambdaKeywords, &kDeclarations}) {
        if (const LispLspVocabEntry *e = FindIn(*table, upper_name)) return e;
    }
    return nullptr;
}

const LispLspVocabEntry *LispLspFindLoopKeyword(const std::string &upper_name) {
    return FindIn(kLoopKeywords, upper_name);
}

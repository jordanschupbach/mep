// mep's OCaml language server: the vocabulary. Keywords, and a compiled-in
// subset of the standard library's interfaces (as .mli text with odoc comments,
// read by the same indexer as installed files) for when OCaml itself is not
// installed. When it is, the installed stdlib .mli files are used instead
// (`ocamlfind ocamlc -where` / `ocamlc -where`, see ocaml_lsp_index.cpp).

#include <algorithm>

#include "ocaml_lsp_internal.h"

namespace mllsp {

const std::vector<std::string> &Keywords() {
    static const std::vector<std::string> k = {
        "and",     "as",       "assert", "begin",   "class",   "constraint", "do",      "done",   "downto",
        "else",    "end",      "exception", "external", "false", "for",      "fun",     "function", "functor",
        "if",      "in",       "include", "inherit", "initializer", "lazy",  "let",     "match",  "method",
        "module",  "mutable",  "new",    "nonrec",  "object",  "of",         "open",    "private", "rec",
        "sig",     "struct",   "then",   "to",      "true",    "try",        "type",    "val",    "virtual",
        "when",    "while",    "with",   "lor",     "lxor",    "mod",        "land",    "lsl",    "lsr",
        "asr",     "or",
    };
    return k;
}

bool IsKeyword(std::string_view w) {
    const auto &k = Keywords();
    return std::find(k.begin(), k.end(), w) != k.end();
}

namespace {

const char *const kStdlib = R"ml((** The OCaml Standard library.

    This module is automatically opened at the beginning of each
    compilation. All components of this module can therefore be referred
    by their short name, without prefixing them by [Stdlib]. *)

(** {1 Exceptions} *)

external raise : exn -> 'a = "%raise"
(** Raise the given exception value *)

val invalid_arg : string -> 'a
(** Raise exception [Invalid_argument] with the given string. *)

val failwith : string -> 'a
(** Raise exception [Failure] with the given string. *)

exception Exit
(** The [Exit] exception is not raised by any library function.  It is
    provided for use in your programs. *)

exception Not_found
exception Invalid_argument of string
exception Failure of string
exception Division_by_zero
exception End_of_file

(** {1 Comparisons} *)

external ( = ) : 'a -> 'a -> bool = "%equal"
(** [e1 = e2] tests for structural equality of [e1] and [e2]. *)

external ( <> ) : 'a -> 'a -> bool = "%notequal"
(** Negation of {!Stdlib.( = )}. *)

external ( < ) : 'a -> 'a -> bool = "%lessthan"
external ( > ) : 'a -> 'a -> bool = "%greaterthan"
external ( <= ) : 'a -> 'a -> bool = "%lessequal"
external ( >= ) : 'a -> 'a -> bool = "%greaterequal"

external compare : 'a -> 'a -> int = "%compare"
(** [compare x y] returns [0] if [x] is equal to [y], a negative integer if
    [x] is less than [y], and a positive integer if [x] is greater than [y]. *)

val min : 'a -> 'a -> 'a
(** Return the smaller of the two arguments. *)

val max : 'a -> 'a -> 'a
(** Return the greater of the two arguments. *)

external ( == ) : 'a -> 'a -> bool = "%eq"
(** [e1 == e2] tests for physical equality of [e1] and [e2]. *)

external ( != ) : 'a -> 'a -> bool = "%noteq"

(** {1 Boolean operations} *)

external not : bool -> bool = "%boolnot"
(** The boolean negation. *)

external ( && ) : bool -> bool -> bool = "%sequand"
external ( || ) : bool -> bool -> bool = "%sequor"

(** {1 Composition operators} *)

external ( |> ) : 'a -> ('a -> 'b) -> 'b = "%revapply"
(** Reverse-application operator: [x |> f |> g] is exactly equivalent
    to [g (f (x))]. *)

external ( @@ ) : ('a -> 'b) -> 'a -> 'b = "%apply"
(** Application operator: [g @@ f @@ x] is exactly equivalent to [g (f (x))]. *)

(** {1 Integer arithmetic} *)

external ( ~- ) : int -> int = "%negint"
external ( + ) : int -> int -> int = "%addint"
external ( - ) : int -> int -> int = "%subint"
external ( * ) : int -> int -> int = "%mulint"
external ( / ) : int -> int -> int = "%divint"
external ( mod ) : int -> int -> int = "%modint"

val abs : int -> int
(** [abs x] is the absolute value of [x]. *)

val max_int : int
val min_int : int

external succ : int -> int = "%succint"
(** [succ x] is [x + 1]. *)

external pred : int -> int = "%predint"
(** [pred x] is [x - 1]. *)

(** {1 Floating-point arithmetic} *)

external ( +. ) : float -> float -> float = "%addfloat"
external ( -. ) : float -> float -> float = "%subfloat"
external ( *. ) : float -> float -> float = "%mulfloat"
external ( /. ) : float -> float -> float = "%divfloat"
external ( ** ) : float -> float -> float = "caml_power_float"
external sqrt : float -> float = "caml_sqrt_float"
(** Square root. *)

external exp : float -> float = "caml_exp_float"
external log : float -> float = "caml_log_float"
external sin : float -> float = "caml_sin_float"
external cos : float -> float = "caml_cos_float"
external floor : float -> float = "caml_floor_float"
external ceil : float -> float = "caml_ceil_float"
external abs_float : float -> float = "%absfloat"
external float_of_int : int -> float = "%floatofint"
(** Convert an integer to floating-point. *)

external float : int -> float = "%floatofint"
(** Same as {!float_of_int}. *)

external truncate : float -> int = "%intoffloat"
external int_of_float : float -> int = "%intoffloat"
(** Truncate the given floating-point number to an integer. *)

val infinity : float
val nan : float

(** {1 String operations} *)

val ( ^ ) : string -> string -> string
(** String concatenation. *)

(** {1 Character operations} *)

external int_of_char : char -> int = "%identity"
(** Return the ASCII code of the argument. *)

val char_of_int : int -> char
(** Return the character with the given ASCII code.
    @raise Invalid_argument if the argument is outside the range 0--255. *)

(** {1 Unit operations} *)

external ignore : 'a -> unit = "%ignore"
(** Discard the value of its argument and return [()]. *)

(** {1 String conversion functions} *)

val string_of_bool : bool -> string
val bool_of_string : string -> bool
val string_of_int : int -> string
(** Return the string representation of an integer, in decimal. *)

external int_of_string : string -> int = "caml_int_of_string"
(** Convert the given string to an integer.
    @raise Failure if the given string is not a valid representation of an integer. *)

val int_of_string_opt : string -> int option
val string_of_float : float -> string
external float_of_string : string -> float = "caml_float_of_string"
val float_of_string_opt : string -> float option

(** {1 Pair operations} *)

external fst : 'a * 'b -> 'a = "%field0"
(** Return the first component of a pair. *)

external snd : 'a * 'b -> 'b = "%field1"
(** Return the second component of a pair. *)

(** {1 List operations} *)

val ( @ ) : 'a list -> 'a list -> 'a list
(** [l0 @ l1] appends [l1] to [l0]. *)

(** {1 Output functions on standard output} *)

val print_char : char -> unit
val print_string : string -> unit
(** Print a string on standard output. *)

val print_int : int -> unit
val print_float : float -> unit
val print_endline : string -> unit
(** Print a string, followed by a newline character, on
    standard output and flush standard output. *)

val print_newline : unit -> unit

(** {1 Output functions on standard error} *)

val prerr_string : string -> unit
val prerr_endline : string -> unit
(** Print a string, followed by a newline character on standard
    error and flush standard error. *)

(** {1 Input functions on standard input} *)

val read_line : unit -> string
(** Flush standard output, then read characters from standard input
    until a newline character is encountered. *)

val read_int : unit -> int

(** {1 References} *)

type 'a ref = { mutable contents : 'a }
(** The type of references (mutable indirection cells) containing
    a value of type ['a]. *)

external ref : 'a -> 'a ref = "%makemutable"
(** Return a fresh reference containing the given value. *)

external ( ! ) : 'a ref -> 'a = "%field0"
(** [!r] returns the current contents of reference [r]. *)

external ( := ) : 'a ref -> 'a -> unit = "%setfield0"
(** [r := a] stores the value of [a] in reference [r]. *)

external incr : int ref -> unit = "%incr"
(** Increment the integer contained in the given reference. *)

external decr : int ref -> unit = "%decr"

(** {1 Result type} *)

type ('a, 'b) result = Ok of 'a | Error of 'b

(** {1 Program termination} *)

val exit : int -> 'a
(** Terminate the process, returning the given status code to the
    operating system. *)

val at_exit : (unit -> unit) -> unit

module Array = Stdlib__Array
module Buffer = Stdlib__Buffer
module Char = Stdlib__Char
module Filename = Stdlib__Filename
module Format = Stdlib__Format
module Fun = Stdlib__Fun
module Hashtbl = Stdlib__Hashtbl
module Int = Stdlib__Int
module List = Stdlib__List
module Map = Stdlib__Map
module Option = Stdlib__Option
module Printf = Stdlib__Printf
module Result = Stdlib__Result
module Seq = Stdlib__Seq
module Set = Stdlib__Set
module String = Stdlib__String
module Sys = Stdlib__Sys
)ml";

const char *const kList = R"ml((** List operations.

    Some functions are flagged as not tail-recursive. *)

type 'a t = 'a list = [] | (::) of 'a * 'a list

val length : 'a list -> int
(** Return the length (number of elements) of the given list. *)

val hd : 'a list -> 'a
(** Return the first element of the given list.
    @raise Failure if the list is empty. *)

val tl : 'a list -> 'a list
(** Return the given list without its first element.
    @raise Failure if the list is empty. *)

val nth : 'a list -> int -> 'a
(** Return the [n]-th element of the given list.
    @raise Failure if the list is too short. *)

val rev : 'a list -> 'a list
(** List reversal. *)

val append : 'a list -> 'a list -> 'a list
(** [append l0 l1] appends [l1] to [l0]. Same function as the infix operator [@]. *)

val concat : 'a list list -> 'a list
(** Concatenate a list of lists. *)

val iter : ('a -> unit) -> 'a list -> unit
(** [iter f [a1; ...; an]] applies function [f] in turn to [[a1; ...; an]]. *)

val iteri : (int -> 'a -> unit) -> 'a list -> unit

val map : ('a -> 'b) -> 'a list -> 'b list
(** [map f [a1; ...; an]] applies function [f] to [a1, ..., an],
    and builds the list [[f a1; ...; f an]] with the results returned by [f]. *)

val mapi : (int -> 'a -> 'b) -> 'a list -> 'b list
val filter_map : ('a -> 'b option) -> 'a list -> 'b list
val concat_map : ('a -> 'b list) -> 'a list -> 'b list

val fold_left : ('acc -> 'a -> 'acc) -> 'acc -> 'a list -> 'acc
(** [fold_left f init [b1; ...; bn]] is [f (... (f (f init b1) b2) ...) bn]. *)

val fold_right : ('a -> 'acc -> 'acc) -> 'a list -> 'acc -> 'acc
(** [fold_right f [a1; ...; an] init] is [f a1 (f a2 (... (f an init) ...))].
    Not tail-recursive. *)

val for_all : ('a -> bool) -> 'a list -> bool
val exists : ('a -> bool) -> 'a list -> bool
val mem : 'a -> 'a list -> bool

val find : ('a -> bool) -> 'a list -> 'a
(** [find f l] returns the first element of the list [l] that satisfies the predicate [f].
    @raise Not_found if there is no value that satisfies [f] in the list [l]. *)

val find_opt : ('a -> bool) -> 'a list -> 'a option

val filter : ('a -> bool) -> 'a list -> 'a list
(** [filter f l] returns all the elements of the list [l] that satisfy the predicate [f]. *)

val partition : ('a -> bool) -> 'a list -> 'a list * 'a list
val assoc : 'a -> ('a * 'b) list -> 'b
val assoc_opt : 'a -> ('a * 'b) list -> 'b option
val combine : 'a list -> 'b list -> ('a * 'b) list
val split : ('a * 'b) list -> 'a list * 'b list

val sort : ('a -> 'a -> int) -> 'a list -> 'a list
(** Sort a list in increasing order according to a comparison function. *)

val init : int -> (int -> 'a) -> 'a list
val to_seq : 'a list -> 'a Seq.t
val of_seq : 'a Seq.t -> 'a list
)ml";

const char *const kString = R"ml((** Strings. *)

type t = string

val make : int -> char -> string
val init : int -> (int -> char) -> string
val length : string -> int
(** [length s] is the length (number of bytes/characters) of [s]. *)

val get : string -> int -> char
val sub : string -> int -> int -> string
(** [sub s pos len] is a string of length [len], containing the
    substring of [s] that starts at position [pos] and has length [len]. *)

val concat : string -> string list -> string
(** [concat sep ss] concatenates the list of strings [ss], inserting
    the separator string [sep] between each. *)

val equal : t -> t -> bool
val compare : t -> t -> int
val starts_with : prefix:string -> string -> bool
val ends_with : suffix:string -> string -> bool
val contains : string -> char -> bool
val index_opt : string -> char -> int option
val split_on_char : char -> string -> string list
(** [split_on_char sep s] is the list of all (possibly empty)
    substrings of [s] that are delimited by the character [sep]. *)

val map : (char -> char) -> string -> string
val iter : (char -> unit) -> string -> unit
val trim : string -> string
(** [trim s] is [s] without leading and trailing whitespace. *)

val uppercase_ascii : string -> string
val lowercase_ascii : string -> string
val capitalize_ascii : string -> string
)ml";

const char *const kArray = R"ml((** Array operations. *)

type 'a t = 'a array

external length : 'a array -> int = "%array_length"
external get : 'a array -> int -> 'a = "%array_safe_get"
external set : 'a array -> int -> 'a -> unit = "%array_safe_set"
external make : int -> 'a -> 'a array = "caml_make_vect"
val init : int -> (int -> 'a) -> 'a array
val append : 'a array -> 'a array -> 'a array
val sub : 'a array -> int -> int -> 'a array
val copy : 'a array -> 'a array
val to_list : 'a array -> 'a list
val of_list : 'a list -> 'a array
val iter : ('a -> unit) -> 'a array -> unit
val iteri : (int -> 'a -> unit) -> 'a array -> unit
val map : ('a -> 'b) -> 'a array -> 'b array
val mapi : (int -> 'a -> 'b) -> 'a array -> 'b array
val fold_left : ('acc -> 'a -> 'acc) -> 'acc -> 'a array -> 'acc
val fold_right : ('a -> 'acc -> 'acc) -> 'a array -> 'acc -> 'acc
val exists : ('a -> bool) -> 'a array -> bool
val for_all : ('a -> bool) -> 'a array -> bool
val mem : 'a -> 'a array -> bool
val sort : ('a -> 'a -> int) -> 'a array -> unit
)ml";

const char *const kPrintf = R"ml((** Formatted output functions. *)

val fprintf : out_channel -> ('a, out_channel, unit) format -> 'a
val printf : ('a, out_channel, unit) format -> 'a
(** Same as {!Printf.fprintf}, but output on [stdout]. *)

val eprintf : ('a, out_channel, unit) format -> 'a
(** Same as {!Printf.fprintf}, but output on [stderr]. *)

val sprintf : ('a, unit, string) format -> 'a
(** Same as {!Printf.fprintf}, but instead of printing on an output channel,
    return a string containing the result of formatting the arguments. *)
)ml";

const char *const kOption = R"ml((** Option values. *)

type 'a t = 'a option = None | Some of 'a

val none : 'a option
val some : 'a -> 'a option
val value : 'a option -> default:'a -> 'a
(** [value o ~default] is [v] if [o] is [Some v] and [default] otherwise. *)

val get : 'a option -> 'a
val bind : 'a option -> ('a -> 'b option) -> 'b option
val map : ('a -> 'b) -> 'a option -> 'b option
val fold : none:'a -> some:('b -> 'a) -> 'b option -> 'a
val iter : ('a -> unit) -> 'a option -> unit
val is_none : 'a option -> bool
val is_some : 'a option -> bool
val to_list : 'a option -> 'a list
)ml";

const char *const kHashtbl = R"ml((** Hash tables and hash functions. *)

type (!'a, !'b) t

val create : ?random:bool -> int -> ('a, 'b) t
(** [Hashtbl.create n] creates a new, empty hash table, with
    initial size greater or equal to the suggested size [n]. *)

val clear : ('a, 'b) t -> unit
val add : ('a, 'b) t -> 'a -> 'b -> unit
val find : ('a, 'b) t -> 'a -> 'b
val find_opt : ('a, 'b) t -> 'a -> 'b option
val mem : ('a, 'b) t -> 'a -> bool
val remove : ('a, 'b) t -> 'a -> unit
val replace : ('a, 'b) t -> 'a -> 'b -> unit
val iter : ('a -> 'b -> unit) -> ('a, 'b) t -> unit
val fold : ('a -> 'b -> 'acc -> 'acc) -> ('a, 'b) t -> 'acc -> 'acc
val length : ('a, 'b) t -> int
)ml";

}  // namespace

const std::vector<std::pair<std::string, std::string>> &FallbackModules() {
    static const std::vector<std::pair<std::string, std::string>> m = {
        {"Stdlib", kStdlib}, {"List", kList},     {"String", kString},   {"Array", kArray},
        {"Printf", kPrintf}, {"Option", kOption}, {"Hashtbl", kHashtbl},
    };
    return m;
}

}  // namespace mllsp

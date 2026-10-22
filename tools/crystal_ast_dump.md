# crystal-ast-dump output format

`tools/crystal_ast_dump.cr` (build: `just crystal-ast-dump`) prints the AST
the real Crystal parser builds, so that mep's C++ port of
`src/compiler/crystal/syntax/{lexer,parser,ast}.cr` can print the same text
and the two can be diffed byte for byte. This is the spec for that printer.
Everything is UTF-8, lines end in `\n`, there is no trailing whitespace.

## Per file

```
=== <path as given on the command line / stdin line>
<dump of the root node>                    -- if the parse succeeded
ERROR <line>:<column> <message>            -- if it raised Crystal::SyntaxException
WARNING <line>:<column> <message>          -- zero or more, after either of the above
```

* The parse is the compiler's: `Parser.new(source)`, `filename = path`,
  `wants_doc = false` (the default for `crystal build`; `crystal tool format`
  does the same minus the filename), `parse`. The source is the file's bytes
  (`File.read`), no BOM stripping, no newline normalisation.
* `ERROR`: `line`/`column` are `SyntaxException#line_number` /
  `#column_number` (1-based; column counts characters, i.e. codepoints, the
  way the lexer's `column_number` does). `message` is the first line of the
  exception message, verbatim (no escaping). Exactly one ERROR line; no partial
  tree is printed.
* `ERROR ?:? <message>`: the file could not be read, or is not valid UTF-8
  (`InvalidByteSequenceError` from the lexer -- what `crystal build` reports as
  "file '...' is not a valid Crystal source file: <message>").
* `CRASH <ExceptionClass>: <first message line>`: any other exception (a parser
  bug). Never happened on the stdlib/compiler/spec tree. Exit status 2 if any.
* `WARNING`: every `warnings.add_warning_at(location, message)` the parser
  made, in order, also when the parse then failed (they come from
  `WarningCollection`, which the tool patches to record them raw). Location
  as in the node ranges below (`?` if nil). `--no-warnings` suppresses them.
  Current parser warnings: "space required before colon in type declaration
  (run `crystal tool format` to fix this)" and its "...in type restriction" /
  "...in return type restriction" variants, "macro fresh variables with
  constant names are deprecated", "single-letter macro fresh variables with
  indices are deprecated", "invalid parameter name: <name>".

## Node lines

Preorder, one node per line, indented two spaces per depth (root depth 0):

```
<indent>[<label>: ]<ClassName> <range>[ <name>=<scalar>]...
```

* `ClassName`: the Crystal class name with the leading `Crystal::` removed:
  `Call`, `ClassDef`, `HashLiteral::Entry`.
* `range`: `<loc>-<loc>` = `location` and `end_location`, each `line:column`
  (1-based, as stored -- the parser sometimes stores column 0, e.g. a
  `MacroLiteral` ending at `4:0`) or `?` if nil (e.g. `Arg 8:18-?`, the
  pieces of a `StringInterpolation` are `?-?`). The root node has no label.
* Then the node's **scalar** fields, then (on the following lines, depth+1)
  its **child** fields. Both are emitted in the class's instance-variable
  declaration order (inherited ivars first) -- see the schema below, which is
  exactly that order, already minus the excluded ivars. Scalars all come
  before any child, so a class's line is its scalars in schema order, and its
  children follow in schema order.

### Excluded ivars

| ivar | why |
|---|---|
| `ASTNode#location`, `#end_location` | printed as the range |
| `#doc` (19 classes) | doc comments; always nil, the compiler parses with `wants_doc = false` |
| `Block#call` | back-reference to the Call owning the block (a cycle) |
| `Call#expansion` | only set by the semantic phase |
| `Def#macro_def` | only set by the semantic phase |

Semantic-phase ivars (`@type`, `@dependencies`, `@observers`, bindings...)
don't exist at all: the tool loads only `compiler/crystal/syntax`, and those
are added by `compiler/crystal/semantic/*.cr`. `Call#name_size` IS printed:
it is the raw ivar (-1 = not computed yet), and the parser itself stores 0 in
it for `a[b] = c`, `a[b]?`-style index calls.

### Scalars: ` <name>=<value>`

A field is a scalar if its value is one of these (decided by the runtime value,
so a nil-able scalar that is nil is simply omitted):

| type | printed as |
|---|---|
| `String` | `"..."` -- see escaping |
| `Char` | `'x'` -- same escaping, with `'` escaped instead of `"` |
| `Bool` | `true` / `false` |
| `Int32` etc. | decimal (`splat_index=1`, `name_size=-1`, `heredoc_indent=2`) |
| `Array(String)` | `["a", "b"]` (elements escaped as strings, `", "` separated; an empty array prints `[]`) |
| `Location` | `line:column` (`name_location`, `else_location`, `ensure_location`) |
| `Regex::Options` (a `@[Flags]` enum) | its integer value: `i`=1, `m`=6, `x`=8 -> `/re/imx` is `options=15`, none is `0` |
| `NumberKind` | lower-case: `i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 f32 f64` |
| `Visibility` | `Public` `Protected` `Private` |
| `Expressions::Keyword` | `None` `Paren` `Begin` |
| `Generic::Suffix` | `None` `Question` `Asterisk` `Bracket` |
| `Token::Kind` (`MagicConstant#name`) | `__DIR__` `__END_LINE__` `__FILE__` `__LINE__` |

**String escaping** (deliberately simpler than Crystal's `String#inspect`, which
depends on Unicode printability tables): `"` -> `\"`, `\` -> `\\`,
newline -> `\n`, tab -> `\t`, CR -> `\r`, any other codepoint < 0x20 and 0x7F
-> `\xHH` (two upper-case hex digits); each byte of an invalid UTF-8 sequence
(as Crystal's `Char::Reader` delimits it) -> `\xHH`; a surrogate codepoint
(possible only in a `Char`) -> `\u{HHHH}`; everything else, non-ASCII
included, is copied as UTF-8. `#` is not escaped.

### Children: `<label>: <node line>`

| value | printed as |
|---|---|
| nil | nothing |
| `ASTNode` | one node, label = field name: `body: Expressions ...` |
| `Array` of nodes | one node per element, label `field[i]` (0-based); empty array -> nothing |
| `Hash(Int32, Expressions)` (`Block#unpacks`) | one node per entry in insertion order, label `field{key}`: `unpacks{0}: Expressions ...` |
| `HashLiteral::Entry`, `NamedTupleLiteral::Entry` | a pseudo-node: same line format but **no range** (they are records, not ASTNodes), then their own fields by the same rules: `entries[0]: HashLiteral::Entry` with children `key:`/`value:`; `entries[1]: NamedTupleLiteral::Entry key="b c"` with child `value:`; `HashLiteral#of` is `of: HashLiteral::Entry` |

If the parser ever stores one node object in two places, it is printed at
each place (the output is a tree, never a graph).

## Example

```crystal
class Greeter(T) < Base
  include Comparable(Greeter)
  @count = 0_u8

  def greet(name : String = "world", *, loud = false) : String?
    msg = "Hi, #{name}!"
    loud ? msg.upcase : msg
  end
end
%w(a b).each_with_index { |s, i| puts s * i }
```

```
=== sample.cr
Expressions 1:1-10:45 keyword=None
  expressions[0]: ClassDef 1:1-9:3 type_vars=["T"] name_location=1:7 abstract=false struct=false visibility=Public
    name: Path 1:7-1:13 names=["Greeter"] global=false visibility=Public
    body: Expressions 2:3-8:5 keyword=None
      expressions[0]: Include 2:3-2:29
        name: Generic 2:11-2:29 suffix=None
          name: Path 2:11-2:20 names=["Comparable"] global=false visibility=Public
          type_vars[0]: Path 2:22-2:28 names=["Greeter"] global=false visibility=Public
      expressions[1]: Assign 3:3-3:15
        target: InstanceVar 3:3-3:8 name="@count"
        value: NumberLiteral 3:12-3:15 value="0" kind=u8
      expressions[2]: Def 5:3-8:5 name="greet" name_location=5:7 splat_index=1 abstract=false visibility=Public calls_super=false calls_initialize=false calls_previous_def=false uses_block_arg=false assigns_special_var=false
        args[0]: Arg 5:13-? name="name" external_name="name"
          default_value: StringLiteral 5:29-5:35 value="world"
          restriction: Path 5:20-5:25 names=["String"] global=false visibility=Public
        args[1]: Arg 5:38-? name="" external_name=""
        args[2]: Arg 5:41-? name="loud" external_name="loud"
          default_value: BoolLiteral 5:48-5:52 value=false
        body: Expressions 6:5-7:27 keyword=None
          expressions[0]: Assign 6:5-6:24
            target: Var 6:5-6:7 name="msg"
            value: StringInterpolation 6:11-6:24 heredoc_indent=0
              expressions[0]: StringLiteral ?-? value="Hi, "
              expressions[1]: Var 6:18-6:21 name="name"
              expressions[2]: StringLiteral ?-? value="!"
          expressions[1]: If 7:5-7:27 ternary=true
            cond: Var 7:5-7:8 name="loud"
            then: Call 7:12-7:21 name="upcase" name_location=7:16 global=false name_size=-1 visibility=Public args_in_brackets=false has_parentheses=false
              obj: Var 7:12-7:14 name="msg"
            else: Var 7:25-7:27 name="msg"
        return_type: Union 5:57-5:63
          types[0]: Path 5:57-5:62 names=["String"] global=false visibility=Public
          types[1]: Path 5:57-5:62 names=["Nil"] global=true visibility=Public
    superclass: Path 1:20-1:23 names=["Base"] global=false visibility=Public
  expressions[1]: Call 10:1-10:45 name="each_with_index" name_location=10:9 global=false name_size=-1 visibility=Public args_in_brackets=false has_parentheses=false
    obj: ArrayLiteral 10:1-10:7
      elements[0]: StringLiteral 10:4-10:4 value="a"
      elements[1]: StringLiteral 10:6-10:6 value="b"
      of: Path ?-? names=["String"] global=true visibility=Public
    block: Block 10:25-10:45
      args[0]: Var 10:28-? name="s"
      args[1]: Var 10:31-? name="i"
      body: Call 10:34-10:43 name="puts" name_location=10:34 global=false name_size=-1 visibility=Public args_in_brackets=false has_parentheses=false
        args[0]: Call 10:39-10:43 name="*" name_location=10:41 global=false name_size=-1 visibility=Public args_in_brackets=false has_parentheses=false
          obj: Var 10:39-10:39 name="s"
          args[0]: Var 10:43-10:43 name="i"
```

## Schema

Output of `crystal-ast-dump --schema` for Crystal 1.19.1: every concrete node
class (and the two Entry records), and its dumped fields in output order with
their declared types. A type containing `ASTNode`/a node class/`Entry`/`Hash`
is a child field; everything else is a scalar.

```
Alias
  name : Path
  value : ASTNode
  visibility : Visibility
AlignOf
  exp : ASTNode
And
  left : ASTNode
  right : ASTNode
Annotation
  path : Path
  args : Array(ASTNode)
  named_args : (Array(NamedArgument) | Nil)
AnnotationDef
  name : Path
  name_location : (Location | Nil)
Arg
  name : String
  external_name : String
  default_value : (ASTNode | Nil)
  restriction : (ASTNode | Nil)
  parsed_annotations : (Array(Annotation) | Nil)
ArrayLiteral
  elements : Array(ASTNode)
  of : (ASTNode | Nil)
  name : (ASTNode | Nil)
Asm
  text : String
  outputs : (Array(AsmOperand) | Nil)
  inputs : (Array(AsmOperand) | Nil)
  clobbers : (Array(String) | Nil)
  volatile : Bool
  alignstack : Bool
  intel : Bool
  can_throw : Bool
AsmOperand
  constraint : String
  exp : ASTNode
Assign
  target : ASTNode
  value : ASTNode
Block
  args : Array(Var)
  body : ASTNode
  splat_index : (Int32 | Nil)
  unpacks : (Hash(Int32, Expressions) | Nil)
BoolLiteral
  value : Bool
Break
  exp : (ASTNode | Nil)
CStructOrUnionDef
  name : String
  body : ASTNode
  union : Bool
Call
  obj : (ASTNode | Nil)
  name : String
  args : Array(ASTNode)
  block : (Block | Nil)
  block_arg : (ASTNode | Nil)
  named_args : (Array(NamedArgument) | Nil)
  name_location : (Location | Nil)
  global : Bool
  name_size : Int32
  visibility : Visibility
  args_in_brackets : Bool
  has_parentheses : Bool
Case
  cond : (ASTNode | Nil)
  whens : Array(When)
  else : (ASTNode | Nil)
  exhaustive : Bool
Cast
  obj : ASTNode
  to : ASTNode
CharLiteral
  value : Char
ClassDef
  name : Path
  body : ASTNode
  superclass : (ASTNode | Nil)
  type_vars : (Array(String) | Nil)
  name_location : (Location | Nil)
  splat_index : (Int32 | Nil)
  abstract : Bool
  struct : Bool
  visibility : Visibility
ClassVar
  name : String
Def
  free_vars : (Array(String) | Nil)
  receiver : (ASTNode | Nil)
  name : String
  args : Array(Arg)
  double_splat : (Arg | Nil)
  body : ASTNode
  block_arg : (Arg | Nil)
  return_type : (ASTNode | Nil)
  block_arity : (Int32 | Nil)
  name_location : (Location | Nil)
  splat_index : (Int32 | Nil)
  abstract : Bool
  visibility : Visibility
  calls_super : Bool
  calls_initialize : Bool
  calls_previous_def : Bool
  uses_block_arg : Bool
  assigns_special_var : Bool
DoubleSplat
  exp : ASTNode
EnumDef
  name : Path
  members : Array(ASTNode)
  base_type : (ASTNode | Nil)
  visibility : Visibility
ExceptionHandler
  body : ASTNode
  rescues : (Array(Rescue) | Nil)
  else : (ASTNode | Nil)
  ensure : (ASTNode | Nil)
  else_location : (Location | Nil)
  ensure_location : (Location | Nil)
  implicit : Bool
  suffix : Bool
Expressions
  expressions : Array(ASTNode)
  keyword : Expressions::Keyword
Extend
  name : ASTNode
ExternalVar
  name : String
  type_spec : ASTNode
  real_name : (String | Nil)
FunDef
  name : String
  args : Array(Arg)
  return_type : (ASTNode | Nil)
  body : (ASTNode | Nil)
  real_name : String
  varargs : Bool
  name_location : (Location | Nil)
Generic
  name : ASTNode
  type_vars : Array(ASTNode)
  named_args : (Array(NamedArgument) | Nil)
  suffix : Generic::Suffix
Global
  name : String
HashLiteral
  entries : Array(HashLiteral::Entry)
  of : (HashLiteral::Entry | Nil)
  name : (ASTNode | Nil)
HashLiteral::Entry
  key : ASTNode
  value : ASTNode
If
  cond : ASTNode
  then : ASTNode
  else : ASTNode
  ternary : Bool
  else_location : (Location | Nil)
ImplicitObj
Include
  name : ASTNode
InstanceAlignOf
  exp : ASTNode
InstanceSizeOf
  exp : ASTNode
InstanceVar
  name : String
IsA
  obj : ASTNode
  const : ASTNode
  nil_check : Bool
LibDef
  name : Path
  body : ASTNode
  name_location : (Location | Nil)
  visibility : Visibility
Macro
  name : String
  args : Array(Arg)
  body : ASTNode
  double_splat : (Arg | Nil)
  block_arg : (Arg | Nil)
  name_location : (Location | Nil)
  splat_index : (Int32 | Nil)
  visibility : Visibility
MacroExpression
  exp : ASTNode
  output : Bool
MacroFor
  vars : Array(Var)
  exp : ASTNode
  body : ASTNode
MacroIf
  cond : ASTNode
  then : ASTNode
  else : ASTNode
  is_unless : Bool
MacroLiteral
  value : String
MacroVar
  name : String
  exps : (Array(ASTNode) | Nil)
MacroVerbatim
  exp : ASTNode
MagicConstant
  name : Token::Kind
Metaclass
  name : ASTNode
ModuleDef
  name : Path
  body : ASTNode
  type_vars : (Array(String) | Nil)
  splat_index : (Int32 | Nil)
  name_location : (Location | Nil)
  visibility : Visibility
MultiAssign
  targets : Array(ASTNode)
  values : Array(ASTNode)
NamedArgument
  name : String
  value : ASTNode
NamedTupleLiteral
  entries : Array(NamedTupleLiteral::Entry)
NamedTupleLiteral::Entry
  key : String
  value : ASTNode
Next
  exp : (ASTNode | Nil)
NilLiteral
NilableCast
  obj : ASTNode
  to : ASTNode
Nop
Not
  exp : ASTNode
NumberLiteral
  value : String
  kind : NumberKind
OffsetOf
  offsetof_type : ASTNode
  offset : ASTNode
OpAssign
  target : ASTNode
  op : String
  value : ASTNode
  name_location : (Location | Nil)
Or
  left : ASTNode
  right : ASTNode
Out
  exp : ASTNode
Path
  names : Array(String)
  global : Bool
  visibility : Visibility
PointerOf
  exp : ASTNode
ProcLiteral
  def : Def
ProcNotation
  inputs : (Array(ASTNode) | Nil)
  output : (ASTNode | Nil)
ProcPointer
  obj : (ASTNode | Nil)
  name : String
  args : Array(ASTNode)
  global : Bool
RangeLiteral
  from : ASTNode
  to : ASTNode
  exclusive : Bool
ReadInstanceVar
  obj : ASTNode
  name : String
RegexLiteral
  value : ASTNode
  options : Regex::Options
Require
  string : String
Rescue
  body : ASTNode
  types : (Array(ASTNode) | Nil)
  name : (String | Nil)
RespondsTo
  obj : ASTNode
  name : String
Return
  exp : (ASTNode | Nil)
Select
  whens : Array(When)
  else : (ASTNode | Nil)
Self
SizeOf
  exp : ASTNode
Splat
  exp : ASTNode
StringInterpolation
  expressions : Array(ASTNode)
  heredoc_indent : Int32
StringLiteral
  value : String
SymbolLiteral
  value : String
TupleLiteral
  elements : Array(ASTNode)
TypeDeclaration
  var : ASTNode
  declared_type : ASTNode
  value : (ASTNode | Nil)
TypeDef
  name : String
  type_spec : ASTNode
  name_location : (Location | Nil)
TypeOf
  expressions : Array(ASTNode)
Underscore
UninitializedVar
  var : ASTNode
  declared_type : ASTNode
Union
  types : Array(ASTNode)
Unless
  cond : ASTNode
  then : ASTNode
  else : ASTNode
  else_location : (Location | Nil)
Until
  cond : ASTNode
  body : ASTNode
Var
  name : String
VisibilityModifier
  modifier : Visibility
  exp : ASTNode
When
  conds : Array(ASTNode)
  body : ASTNode
  exhaustive : Bool
While
  cond : ASTNode
  body : ASTNode
Yield
  exps : Array(ASTNode)
  scope : (ASTNode | Nil)
  has_parentheses : Bool
```

# crystal_ast_dump -- the parser oracle for mep's C++ port of Crystal's
# lexer/parser (src/compiler/crystal/syntax/{lexer,parser,ast}.cr).
#
# It parses each file exactly as `crystal build` / `crystal tool format` do
# (Crystal::Parser.new(source); parser.filename = path; parser.wants_doc =
# false -- the compiler's default, only `crystal docs` turns it on;
# parser.parse) and prints the resulting AST in a generic, deterministic
# one-node-per-line format that the C++ port prints too, so the two can be
# diffed file by file. The format is specified in tools/crystal_ast_dump.md.
#
# Build (from the repository root; `just crystal-ast-dump` does this):
#
#   env -u LD_LIBRARY_PATH crystal build --release --no-debug \
#       tools/crystal_ast_dump.cr -o build/crystal-ast-dump/crystal-ast-dump
#
# `require "compiler/crystal/syntax"` resolves against the default
# CRYSTAL_PATH: the installed stdlib directory (the `lib:<dir>` part of
# `crystal env CRYSTAL_PATH`) ships the compiler sources too, and they are the
# ones the running `crystal` was built from. To dump with a different
# checkout's parser instead, prepend it:
#
#   CRYSTAL_PATH=/dev/shm/crystal-src/src:$(crystal env CRYSTAL_PATH) crystal build ...
#
# (the two must be the same version: the checkout's stdlib then shadows the
# installed one as well). Only the syntax module is loaded, so none of the
# semantic-phase ivars (types, bindings, dependencies, observers, ...) that
# compiler/crystal/semantic/*.cr add to the AST classes even exist here.
#
# Usage:
#
#   crystal-ast-dump FILE...             dump each file
#   crystal-ast-dump --stdin-list        read file paths, one per line, from stdin
#   crystal-ast-dump --schema            print every node class and its dumped
#                                        fields in order (the C++ printer's spec)
#   crystal-ast-dump --no-warnings ...   omit the WARNING lines
#
# Per file: a header `=== <path>`, then either the dump of the parsed tree or
# exactly one line `ERROR <line>:<column> <message>` (Crystal::SyntaxException,
# first line of its message), then any parser warnings as
# `WARNING <line>:<column> <message>`. An unreadable file, or one that is
# not valid UTF-8 (InvalidByteSequenceError, which `crystal build` reports
# as "file is not a valid Crystal source file"), prints
# `ERROR ?:? <exception message>`; any other exception (a parser crash -- the
# compiler would report it as a bug) prints `CRASH <class>: <message>`.
# Exit status: 0 unless some file CRASHed (then 2).
#
# Ivars EXCLUDED from the dump (everything else in ast.cr is printed):
#
#   ASTNode#location, #end_location   printed once, as the node's range
#   *#doc (19 classes: Var, Call,    doc comments; always nil because the
#     Assign, Def, Macro, ClassDef...) compiler parses with wants_doc = false
#   Block#call                       back-reference to the enclosing Call
#                                    (a cycle; set by Call#block=)
#   Call#expansion                   only ever set by the semantic phase
#   Def#macro_def                    only ever set by the semantic phase
#                                    (the parser always leaves it false)
#
# Kept although they look incidental, because the parser sets them:
# Call#name_size (the raw ivar, -1 = not computed yet; the parser stores 0 for
# `a[b] = c`, `a[b]?` ...), Call#name_location, Def/Macro/ClassDef/...
# #name_location, If/Unless#else_location, ExceptionHandler#else_location and
# #ensure_location (all printed as scalars `L:C`), Def#calls_super and the
# other parser-tracked Def flags.

require "compiler/crystal/syntax"

module Crystal
  # Record warnings raw (location + message), instead of only as the
  # formatted text-with-source-snippet the compiler keeps in #infos.
  class WarningCollection
    getter raw_warnings = [] of {Location?, String}

    def add_warning_at(location : Location?, message : String)
      return unless @level.all?
      @raw_warnings << {location, message}
      previous_def
    end
  end
end

module AstDump
  EXCLUDED = {
    "location", "end_location", "doc", "call", "expansion", "macro_def",
  }

  # --- scalar rendering --------------------------------------------------

  # Strings: double-quoted. Escapes: `"` -> \", `\` -> \\, \n \t \r, any
  # other byte < 0x20 and 0x7F -> \xHH (two uppercase hex digits), and
  # every byte of an invalid UTF-8 sequence -> \xHH. All other characters
  # (including non-ASCII) are copied through as UTF-8.
  def self.quote(io : IO, str : String, q : Char) : Nil
    io << q
    reader = Char::Reader.new(str)
    while reader.has_next?
      if reader.error
        # Char::Reader reports the invalid sequence's length in
        # current_char_width; escape each of its bytes.
        reader.current_char_width.times do |i|
          byte_hex(io, str.to_unsafe[reader.pos + i])
        end
      else
        escape_char(io, reader.current_char, q)
      end
      reader.next_char
    end
    io << q
  end

  def self.escape_char(io : IO, c : Char, q : Char) : Nil
    case c
    when q    then io << '\\' << q
    when '\\' then io << "\\\\"
    when '\n' then io << "\\n"
    when '\t' then io << "\\t"
    when '\r' then io << "\\r"
    else
      if c.ord < 0x20 || c.ord == 0x7F
        byte_hex(io, c.ord.to_u8)
      elsif 0xD800 <= c.ord <= 0xDFFF
        # a surrogate can't be written as UTF-8
        io << "\\u{" << c.ord.to_s(16, upcase: true) << '}'
      else
        io << c
      end
    end
  end

  def self.byte_hex(io : IO, b : UInt8) : Nil
    io << "\\x"
    io << '0' if b < 16
    io << b.to_s(16, upcase: true)
  end

  def self.loc(io : IO, l : Crystal::Location?) : Nil
    if l
      io << l.line_number << ':' << l.column_number
    else
      io << '?'
    end
  end

  # emit_scalar: prints ` name=value` for scalar values; nothing for
  # nodes / node collections / nil (children are printed by emit_child).
  # There is deliberately no catch-all overload: a new ivar type in ast.cr
  # fails to compile here instead of being silently skipped.
  def self.emit_scalar(io, name, v : Nil); end

  def self.emit_scalar(io, name, v : Crystal::ASTNode); end

  def self.emit_scalar(io, name, v : Crystal::HashLiteral::Entry); end

  def self.emit_scalar(io, name, v : Hash); end

  def self.emit_scalar(io, name, v : String)
    io << ' ' << name << '='
    quote(io, v, '"')
  end

  def self.emit_scalar(io, name, v : Char)
    io << ' ' << name << '='
    io << '\''
    escape_char(io, v, '\'')
    io << '\''
  end

  def self.emit_scalar(io, name, v : Bool | Int)
    io << ' ' << name << '=' << v
  end

  def self.emit_scalar(io, name, v : Regex::Options)
    # a @[Flags] enum: printed as its integer value (i=1, m=6, x=8)
    io << ' ' << name << '=' << v.value
  end

  def self.emit_scalar(io, name, v : Enum)
    io << ' ' << name << '=' << v.to_s
  end

  def self.emit_scalar(io, name, v : Crystal::Location)
    io << ' ' << name << '='
    loc(io, v)
  end

  def self.emit_scalar(io, name, v : Array(T)) forall T
    {% if T == String %}
      io << ' ' << name << "=["
      v.each_with_index do |s, i|
        io << ", " if i > 0
        quote(io, s, '"')
      end
      io << ']'
    {% end %}
  end

  def self.emit_scalar(io, name, v : Crystal::NamedTupleLiteral::Entry); end

  # --- children ----------------------------------------------------------

  def self.emit_child(io, depth, label, v : Nil); end

  def self.emit_child(io, depth, label, v : String | Char | Bool | Int | Enum | Crystal::Location); end

  def self.emit_child(io, depth, label, v : Crystal::ASTNode)
    node(io, depth, label, v)
  end

  def self.emit_child(io, depth, label, v : Crystal::HashLiteral::Entry | Crystal::NamedTupleLiteral::Entry)
    node(io, depth, label, v)
  end

  def self.emit_child(io, depth, label, v : Array(T)) forall T
    {% unless T == String %}
      v.each_with_index do |e, i|
        emit_child(io, depth, "#{label}[#{i}]", e)
      end
    {% end %}
  end

  def self.emit_child(io, depth, label, v : Hash)
    v.each do |k, e|
      emit_child(io, depth, "#{label}{#{k}}", e)
    end
  end

  # --- nodes -------------------------------------------------------------

  def self.class_name(v) : String
    v.class.name.lchop("Crystal::")
  end

  def self.node(io : IO, depth : Int32, label : String?, v) : Nil
    depth.times { io << "  " }
    io << label << ": " if label
    io << class_name(v)
    if v.is_a?(Crystal::ASTNode)
      io << ' '
      loc(io, v.location)
      io << '-'
      loc(io, v.end_location)
    end
    v.__dump_fields(io, depth)
  end

  def self.dump(io : IO, root : Crystal::ASTNode) : Nil
    node(io, 0, nil, root)
  end
end

# One generic field walker for every AST class (and the two Entry records):
# `@type.instance_vars` is expanded separately for each concrete type, in
# declaration order (inherited ivars first).
module AstDumpFields
  def __dump_fields(io : IO, depth : Int32) : Nil
    {% begin %}
      {% ivars = @type.instance_vars.reject { |v| AstDump::EXCLUDED.includes?(v.name.stringify) } %}
      {% for v in ivars %}
        AstDump.emit_scalar(io, {{ v.name.stringify }}, @{{ v.name }})
      {% end %}
      io << '\n'
      {% for v in ivars %}
        AstDump.emit_child(io, depth + 1, {{ v.name.stringify }}, @{{ v.name }})
      {% end %}
    {% end %}
  end
end

class Crystal::ASTNode
  include AstDumpFields
end

struct Crystal::HashLiteral::Entry
  include AstDumpFields
end

struct Crystal::NamedTupleLiteral::Entry
  include AstDumpFields
end

def print_schema(io)
  {% begin %}
    {% types = Crystal::ASTNode.all_subclasses.reject(&.abstract?) %}
    {% types = types + [Crystal::HashLiteral::Entry, Crystal::NamedTupleLiteral::Entry] %}
    {% for t in types.sort_by { |t| t.name.stringify } %}
      io << {{ t.name.stringify.gsub(/^Crystal::/, "") }} << '\n'
      {% ivars = t.instance_vars.reject { |v| AstDump::EXCLUDED.includes?(v.name.stringify) } %}
      {% for v in ivars %}
        io << "  " << {{ v.name.stringify }} << " : " << {{ v.type.stringify.gsub(/Crystal::/, "") }} << '\n'
      {% end %}
    {% end %}
  {% end %}
end

def dump_file(io : IO, path : String, warnings : Bool) : Bool
  io << "=== " << path << '\n'
  begin
    source = File.read(path)
  rescue ex
    io << "ERROR ?:? " << ex.message.to_s.lines.first? << '\n'
    return true
  end
  ok = true
  parser = nil
  begin
    parser = Crystal::Parser.new(source)
    parser.filename = path
    parser.wants_doc = false
    AstDump.dump(io, parser.parse)
  rescue ex : InvalidByteSequenceError
    # what `crystal build` reports as "file '...' is not a valid Crystal
    # source file: <message>" (the lexer checks the encoding up front,
    # and again at each char)
    io << "ERROR ?:? " << ex.message << '\n'
  rescue ex : Crystal::SyntaxException
    io << "ERROR " << ex.line_number << ':' << ex.column_number << ' ' << (ex.message.to_s.lines.first? || "") << '\n'
  rescue ex
    io << "CRASH " << ex.class << ": " << (ex.message.to_s.lines.first? || "") << '\n'
    ok = false
  end
  if warnings && parser
    parser.warnings.raw_warnings.each do |(l, msg)|
      io << "WARNING "
      AstDump.loc(io, l)
      io << ' ' << msg << '\n'
    end
  end
  ok
end

paths = [] of String
stdin_list = false
warnings = true
ARGV.each do |arg|
  case arg
  when "--stdin-list"  then stdin_list = true
  when "--no-warnings" then warnings = false
  when "--schema"
    print_schema(STDOUT)
    exit 0
  when "-h", "--help"
    STDERR.puts "usage: crystal-ast-dump [--no-warnings] (--stdin-list | FILE...) | --schema"
    exit 0
  else
    paths << arg
  end
end
if stdin_list
  STDIN.each_line { |l| paths << l unless l.empty? }
end
if paths.empty?
  STDERR.puts "usage: crystal-ast-dump [--no-warnings] (--stdin-list | FILE...) | --schema"
  exit 1
end

buf = IO::Memory.new
all_ok = true
paths.each do |path|
  all_ok = false unless dump_file(buf, path, warnings)
  if buf.bytesize > (1 << 20)
    STDOUT.write(buf.to_slice)
    buf.clear
  end
end
STDOUT.write(buf.to_slice)
STDOUT.flush
exit(all_ok ? 0 : 2)

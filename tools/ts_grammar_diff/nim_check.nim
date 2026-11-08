## nim_check: Nim's own parser (compiler/parser.nim, the one `nim check`
## runs first) over files, nothing else -- no semantic pass, no imports.
## The oracle for `just grammar-diff nim --oracle` and the mep-nim-lsp
## parse check (`just nim-check` builds it).
##
##   nim_check FILE            exit 1 if Nim rejects FILE, its errors on stdout
##   nim_check --batch FILE... one line per file: `path<TAB>OK` or
##                             `path<TAB>line:col: message` (first error), and
##                             with --all every error, `path<TAB>...` each
##
## A file that starts with a `#?` source-filter line (stdtmpl, replace) is
## not Nim until the filter has run: it is reported as `FILTER`, and in
## single-file mode accepted.
import std/[os, strutils]
import compiler/[parser, llstream, idents, options, pathutils, msgs, lineinfos, ast]

type Err = tuple[line, col: int, msg: string]

var errs: seq[Err]

proc handler(conf: ConfigRef; info: TLineInfo; msg: TMsgKind; arg: string) =
  if msg in {errMin..errMax}:
    var text = msgKindToString(msg) % [arg]
    errs.add((int(info.line), int(info.col) + 1, text))

proc check(path: string): seq[Err] =
  errs = @[]
  var src = ""
  try:
    src = readFile(path)
  except IOError:
    return @[(-2, 0, "UNREADABLE")]
  if src.startsWith("#?") or src.startsWith("#!") and src.splitLines().len > 1 and
      src.splitLines()[1].startsWith("#?"):
    return @[(-1, 0, "FILTER")]
  let conf = newConfigRef()
  conf.errorMax = high(int)
  let cache = newIdentCache()
  try:
    discard parseString(src, cache, conf, path, 0, handler)
  except CatchableError as e:
    errs.add((0, 0, "exception: " & e.msg))
  result = errs

proc main() =
  var args = commandLineParams()
  if args.len > 0 and args[0] == "--batch":
    var all = false
    for a in args[1..^1]:
      if a == "--all": all = true; continue
      let e = check(a)
      if e.len == 0: echo a, "\tOK"
      elif e[0].line == -1: echo a, "\tFILTER"
      elif e[0].line == -2: echo a, "\tUNREADABLE"
      elif all:
        for x in e: echo a, "\t", x.line, ":", x.col, ": ", x.msg
      else: echo a, "\t", e[0].line, ":", e[0].col, ": ", e[0].msg
    return
  var bad = false
  for a in args:
    let e = check(a)
    if e.len > 0 and e[0].line >= 0:
      bad = true
      for x in e: echo a, ":", x.line, ":", x.col, ": ", x.msg
  quit(if bad: 1 else: 0)

main()

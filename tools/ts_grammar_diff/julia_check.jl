# Is a file valid Julia? Julia's own parser decides: the file is read with
# JuliaSyntax (the parser `Meta.parseall` uses since Julia 1.10), nothing is
# lowered or run. Prints "LINE:COL: MESSAGE" for the first error diagnostic
# and exits 1; exits 0 when the file parses (warnings don't count).
# Usage: julia --startup-file=no julia_check.jl FILE
const JS = Base.JuliaSyntax
text = read(ARGS[1], String)
stream = JS.ParseStream(text)
JS.parse!(stream; rule=:all)
for d in stream.diagnostics
    d.level == :error || continue
    src = JS.SourceFile(text)
    line, col = JS.source_location(src, d.first_byte)
    println("$line:$col: $(d.message)")
    exit(1)
end
exit(0)

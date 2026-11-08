# Oracle for tools/julia_parse_check.py: dumps what Julia's own parsers make
# of a list of files, in the format `mep-julia-parse-test --tree/--diag`
# prints, so the two can be compared line by line.
#
#   julia --startup-file=no tools/julia_parse_oracle.jl OUTDIR LISTFILE
#
# LISTFILE holds one path per line. For the N-th file (0-based) it writes
#   OUTDIR/N.tree    green tree from JuliaSyntax 0.4.10 (the version Runic.jl
#                    1.5 uses), `build_tree(GreenNode, parse!(stream; rule=:all))`
#   OUTDIR/N.diag04  stream.diagnostics from JuliaSyntax 0.4.10
#   OUTDIR/N.diag    stream.diagnostics from Base.JuliaSyntax (1.0.2 in Julia
#                    1.12, what `Meta.parseall` reports)
# A parser exception is written as a single line `CRASH <message>`.
#
# JuliaSyntax 0.4.10 is found as `JuliaSyntax` if the active project has it,
# else through Runic (`Runic.JuliaSyntax`), as in the runic-env julia.
#
# Tree format: one line per node in preorder,
#   DEPTH \t KIND \t FLAGS \t SPAN \t L|N
# (L = leaf token, N = interior node, even when it has no children).
# Diagnostic format: FIRST_BYTE \t LAST_BYTE \t LEVEL \t MESSAGE (1-based,
# inclusive, as JuliaSyntax stores them; newlines in MESSAGE as `\n`).

const JS04 = try
    @eval import JuliaSyntax
    JuliaSyntax
catch
    @eval import Runic
    Runic.JuliaSyntax
end
const JS10 = Base.JuliaSyntax

function dump_tree(io, node, depth)
    leaf = !JS04.haschildren(node)
    h = JS04.head(node)
    println(io, depth, '\t', string(JS04.kind(h)), '\t', Int(JS04.flags(h)), '\t',
            Int(JS04.span(node)), '\t', leaf ? 'L' : 'N')
    if !leaf
        for c in JS04.children(node)
            dump_tree(io, c, depth + 1)
        end
    end
end

function dump_diags(io, diags)
    for d in diags
        msg = replace(d.message, '\n' => "\\n")
        println(io, d.first_byte, '\t', d.last_byte, '\t', d.level, '\t', msg)
    end
end

function crash_text(e)
    s = sprint(showerror, e)
    replace(first(split(s, '\n')), '\t' => ' ')
end

function main()
    outdir = ARGS[1]
    files = filter(!isempty, readlines(ARGS[2]))
    for (i, f) in enumerate(files)
        n = i - 1
        text = try
            read(f, String)
        catch e
            for ext in ("tree", "diag04", "diag")
                write(joinpath(outdir, "$n.$ext"), "CRASH unreadable\n")
            end
            continue
        end
        open(joinpath(outdir, "$n.tree"), "w") do tio
            open(joinpath(outdir, "$n.diag04"), "w") do dio
                try
                    stream = JS04.ParseStream(text)
                    JS04.parse!(stream; rule=:all)
                    tree = JS04.build_tree(JS04.GreenNode, stream)
                    dump_tree(tio, tree, 0)
                    dump_diags(dio, stream.diagnostics)
                catch e
                    println(tio, "CRASH ", crash_text(e))
                    println(dio, "CRASH ", crash_text(e))
                end
            end
        end
        open(joinpath(outdir, "$n.diag"), "w") do dio
            try
                stream = JS10.ParseStream(text)
                JS10.parse!(stream; rule=:all)
                dump_diags(dio, stream.diagnostics)
            catch e
                println(dio, "CRASH ", crash_text(e))
            end
        end
    end
end

main()

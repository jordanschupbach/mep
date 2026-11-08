# Julia's own verdict on the errors mep-julia-lsp reports from its scope
# analysis (the ones Julia raises when *lowering* code, not parsing it):
# every top-level statement of each file -- and of each `module` inside it
# -- is parsed with Meta.parseall and lowered (Meta.lower) in a scratch
# module; nothing is evaluated. Prints one line per lowering error:
#
#   FILE<TAB>LINE<TAB>MESSAGE
#
# and, at the end, "#stats<TAB>statements<TAB>lowered<TAB>skipped": a
# statement whose lowering throws (it expands a macro the scratch module
# doesn't have) is skipped -- no verdict either way.
#
# Usage: julia --startup-file=no tools/julia_lower_check.jl FILE...   (or `-` + paths on stdin)

files = ARGS == ["-"] ? readlines(stdin) : ARGS
nstmt = 0; nlow = 0; nskip = 0

function check(file, ex, line, mod)
    global nstmt, nlow, nskip
    if ex isa LineNumberNode
        return ex.line
    end
    if ex isa Expr && ex.head === :module
        # (module notbare name body): lower the body's statements in a fresh module.
        m = Module(:LowerCheck)
        l = line
        for a in ex.args[3].args
            l = something(check(file, a, l, m), l)
        end
        return nothing
    end
    if ex isa Expr && ex.head === :toplevel
        l = line
        for a in ex.args
            l = something(check(file, a, l, mod), l)
        end
        return nothing
    end
    nstmt += 1
    lowered = try
        Meta.lower(mod, ex)
    catch
        nskip += 1
        return nothing
    end
    nlow += 1
    if lowered isa Expr && lowered.head === :error
        msg = replace(string(lowered.args[1]), '\n' => ' ')
        println(file, '\t', line, '\t', msg)
    end
    return nothing
end

for file in files
    text = try
        read(file, String)
    catch
        continue
    end
    ex = try
        Meta.parseall(text; filename=file)
    catch
        continue
    end
    # A file Julia can't parse has no lowering verdict.
    any(a -> a isa Expr && a.head in (:error, :incomplete), ex.args) && continue
    check(file, ex, 1, Module(:LowerCheck))
end
println("#stats\t", nstmt, '\t', nlow, '\t', nskip)

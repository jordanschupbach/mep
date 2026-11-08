# Format many Julia files with Runic.jl in ONE julia process and cache the
# results, so tools/format_compare.py (via tools/julia_runic_ref.py) does not
# pay Runic's start-up per file.
#
#   julia tools/julia_runic_cache.jl FILES_TXT [CACHE_DIR]
#
# FILES_TXT lists one path per line. Each file's output goes to
# CACHE_DIR/<sha256 of the input bytes>.out (what `runic FILE` prints: the
# CLI's filemode=true), or CACHE_DIR/<sha256>.err when Runic refuses it
# (parse error, internal assertion). Entries already present are skipped.
# Default CACHE_DIR: build/julia/runic-cache next to this script's repo.

using Runic
using SHA

function main(args)
    list = args[1]
    cache = length(args) >= 2 ? args[2] :
        joinpath(dirname(@__DIR__), "build", "julia", "runic-cache")
    mkpath(cache)
    files = filter(!isempty, readlines(list))
    n_new = 0
    for (k, f) in enumerate(files)
        isfile(f) || continue
        src = read(f, String)
        h = bytes2hex(sha256(src))
        out = joinpath(cache, h * ".out")
        err = joinpath(cache, h * ".err")
        (isfile(out) || isfile(err)) && continue
        try
            fmt = Runic.format_string(src; filemode = true)
            write(out * ".tmp", fmt)
            mv(out * ".tmp", out; force = true)
        catch e
            msg = sprint(showerror, e)
            write(err, first(msg, 2000))
        end
        n_new += 1
        k % 500 == 0 && println(stderr, "  $k/$(length(files))")
    end
    println(stderr, "runic cache: $(length(files)) files, $n_new newly formatted")
    return
end

main(ARGS)

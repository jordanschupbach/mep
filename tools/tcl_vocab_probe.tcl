# Lists Tcl's built-in commands, their subcommands and how many arguments
# each accepts, for tools/gen_tcl_vocab.py. Every command is called in a
# child interpreter (in a scratch directory, the cwd the caller gives) with
# 0..30 and 40 arguments that are all "x": a "wrong # args" error means the
# count is wrong, anything else (success or another error) means Tcl took
# it. The usage line is Tcl's own, from that error.
# Output, one per line (tab-separated):
#   cmd NAME KIND MASK MORE USAGE
#   sub NAME SUBNAME MASK MORE USAGE
#   func NAME MASK            (expr functions, ::tcl::mathfunc; MASK: argument counts 0..4)
#   var NAME                  (variables tclsh starts with)
set skip {exit vwait exec socket cd source load unload after update gets read
    open close fcopy fileevent flush interp coroutine yield yieldto tailcall
    tcl_startOfNextWord tcl_startOfPreviousWord tcl_wordBreakAfter tcl_wordBreakBefore
    tcl_endOfWord auto_execok auto_import auto_load auto_load_index auto_qualify
    auto_mkindex auto_mkindex_old auto_reset tclLog unknown history pkg_mkIndex
    tcl_findLibrary pkg_compareExtension parray tcl_unknown}
set maxn 30

proc probe {words} {
    set i [interp create]
    interp eval $i {proc unknown args {error "unknown command"}}
    set code [catch {interp eval $i $words} msg]
    catch {interp delete $i}
    if {$code == 1 && [string match "wrong # args*" $msg]} {
        return [list 0 $msg]
    }
    return [list 1 $msg]
}

proc arity {prefix} {
    global maxn
    set mask 0
    set usage ""
    for {set n 0} {$n <= $maxn} {incr n} {
        lassign [probe [concat $prefix [lrepeat $n x]]] ok msg
        if {$ok} {
            set mask [expr {$mask | (1 << $n)}]
        } elseif {$usage eq "" || $n > 0} {
            if {[regexp {^wrong # args: should be "(.*)"$} $msg -> u]} { set usage $u }
        }
    }
    lassign [probe [concat $prefix [lrepeat 40 x]]] more msg
    if {!$more && [regexp {^wrong # args: should be "(.*)"$} $msg -> u]} { set usage $u }
    return [list $mask $more $usage]
}

proc subcommands {cmd} {
    set i [interp create]
    set subs {}
    if {[interp eval $i [list namespace ensemble exists ::$cmd]]} {
        set map [interp eval $i [list namespace ensemble configure ::$cmd -map]]
        set subs [dict keys $map]
        if {![llength $subs]} {
            set subs [interp eval $i [list namespace ensemble configure ::$cmd -subcommands]]
        }
        if {![llength $subs]} {
            set ns [interp eval $i [list namespace ensemble configure ::$cmd -namespace]]
            foreach c [interp eval $i [list info commands ${ns}::*]] {
                lappend subs [namespace tail $c]
            }
        }
    }
    if {![llength $subs] && $cmd in {package trace interp after}} {
        catch {interp eval $i [list $cmd __nosuchsub__ x x]} msg
        if {[regexp {(?:bad|unknown or ambiguous) (?:option|subcommand) "__nosuchsub__": must be (.*)$} $msg -> list]} {
            regsub -all {,? or } $list ", " list
            foreach s [split $list ,] { lappend subs [string trim $s] }
        }
    }
    interp delete $i
    return [lsort -unique $subs]
}

set top [interp create]
set cmds [lsort [interp eval $top {info commands ::*}]]
foreach c [lsort [interp eval $top {info commands ::oo::*}]] { lappend cmds $c }
foreach full $cmds {
    set c [string trimleft $full :]
    if {[string match {oo::[A-Z]*} $c] || [string match {oo::define::*} $c] || [string match {oo::objdefine::*} $c]} continue
    set subs [subcommands $c]
    if {$c in $skip} {
        puts "cmd\t$c\t[expr {[llength $subs] ? "ensemble" : "command"}]\t-1\t1\t"
        foreach s $subs { puts "sub\t$c\t$s\t-1\t1\t" }
        continue
    }
    lassign [arity [list $c]] mask more usage
    puts "cmd\t$c\t[expr {[llength $subs] ? "ensemble" : "command"}]\t$mask\t$more\t$usage"
    foreach s $subs {
        if {$s eq "" || [string match *__* $s]} continue
        if {"$c $s" in {"file delete" "file mkdir" "file copy" "file rename" "file link" "chan close" "chan copy" "chan pipe" "chan pop" "chan push" "info frame" "namespace delete" "zlib push" "trace add" "after cancel"}} {
            puts "sub\t$c\t$s\t-1\t1\t"
            continue
        }
        lassign [arity [list $c $s]] mask more usage
        puts "sub\t$c\t$s\t$mask\t$more\t$usage"
    }
}
foreach f [lsort [interp eval $top {info commands ::tcl::mathfunc::*}]] {
    set n [namespace tail $f]
    set mask 0
    for {set k 0} {$k <= 4} {incr k} {
        set a [join [lrepeat $k 1] ,]
        if {![catch {interp eval $top [list expr "$n\($a)"]} msg]
                || ![string match "*arguments for math function*" $msg]} {
            set mask [expr {$mask | (1 << $k)}]
        }
    }
    puts "func\t$n\t$mask"
}
foreach v [lsort [interp eval $top {info globals}]] { puts "var\t$v" }

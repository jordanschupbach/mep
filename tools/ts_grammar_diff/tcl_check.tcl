# Is a file valid Tcl? Tcl's own parser decides: the script is compiled
# (tcl::unsupported::getbytecode), never run. A parse error compiles to a
# `syntax` instruction whose literals are the message and the options
# dict with -errorline. Prints "LINE: MESSAGE" and exits 1 for the first
# one; exits 0 when the file parses. Like `source`, ^Z ends the script.
# Usage: tclsh tcl_check.tcl FILE
set f [open [lindex $argv 0] r]
fconfigure $f -encoding utf-8 -eofchar "\x1a"
set text [read $f]
close $f
set bc [::tcl::unsupported::getbytecode script $text]
set lits [dict get $bc literals]
foreach {pc ins} [dict get $bc instructions] {
    if {[lindex $ins 0] eq "syntax"} {
        # The two pushes before it name the message and the options dict.
        foreach l $lits {
            if {[string match "-code 1 -level 0*" $l]} {
                set opts $l
            }
        }
        set msg [lindex $lits [expr {[lsearch -exact $lits $opts] - 1}]]
        puts "[dict get $opts -errorline]: $msg"
        exit 1
    }
}
exit 0

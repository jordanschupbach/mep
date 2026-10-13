# A small Tcl tour: procs, lists, dicts, and expr math.

proc distance {x1 y1 x2 y2} {
    return [expr {hypot($x1 - $x2, $y1 - $y2)}]
}

proc sum {values} {
    set total 0
    foreach v $values {
        incr total $v
    }
    return $total
}

puts "hello, world"
puts [format "distance = %.2f" [distance 0 0 3 4]]

set nums {1 2 3 4 5}
puts "sum = [sum $nums]"
puts "squares = [lmap n $nums {expr {$n * $n}}]"

set counts [dict create]
foreach word {apple banana avocado cherry} {
    dict incr counts [string index $word 0]
}
dict for {letter n} $counts {
    puts "$letter: $n"
}

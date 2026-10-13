// A small Groovy tour: a class with properties, closures, GStrings, and the
// collection methods Groovy adds to Java's. `groovy` compiles and runs a
// script file in one step.

class Point {
    double x, y

    double distanceTo(Point other) {
        Math.hypot(x - other.x, y - other.y)
    }

    String toString() { "($x, $y)" }
}

def area(Map shape) {
    switch (shape.kind) {
        case 'circle': return Math.PI * shape.r * shape.r
        case 'rect': return shape.w * shape.h
    }
}

println 'hello, world'

def a = new Point(x: 0, y: 0)
def b = new Point(x: 3, y: 4)
printf('distance %s -> %s = %.2f%n', a, b, a.distanceTo(b))

def nums = (1..5).toList()
println "sum = ${nums.sum()}"
println "squares = ${nums.collect { it * it }.join(', ')}"
println "evens = ${nums.findAll { it % 2 == 0 }}"

[[kind: 'circle', r: 1], [kind: 'rect', w: 2, h: 3]].each { shape ->
    printf('%s area = %.2f%n', shape.kind, area(shape) as double)
}

def byLetter = ['apple', 'banana', 'avocado', 'cherry'].countBy { it[0] }
println "by first letter = $byLetter"

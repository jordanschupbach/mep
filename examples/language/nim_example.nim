import std/[math, sequtils, strformat]

type
  Point = object
    x, y: float

proc distanceTo(a, b: Point): float =
  hypot(a.x - b.x, a.y - b.y)

proc sum[T](values: openArray[T]): T =
  for v in values:
    result += v

echo "hello, world"

let a = Point(x: 0.0, y: 0.0)
let b = Point(x: 3.0, y: 4.0)
echo &"distance = {a.distanceTo(b):.2f}"

let nums = @[1, 2, 3, 4, 5]
echo &"sum = {sum(nums)}"
echo "squares = ", nums.mapIt(it * it)

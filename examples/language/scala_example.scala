// A small Scala 3 tour: a case class, an enum with pattern matching, an
// extension method, and collection operations. `scala <file>` compiles and
// runs it.

case class Point(x: Double, y: Double):
  def distanceTo(other: Point): Double = math.hypot(x - other.x, y - other.y)

enum Shape:
  case Circle(r: Double)
  case Rect(w: Double, h: Double)

def area(shape: Shape): Double = shape match
  case Shape.Circle(r)  => math.Pi * r * r
  case Shape.Rect(w, h) => w * h

extension (n: Int) def squared: Int = n * n

@main def scalaExample(): Unit =
  println("hello, world")

  val a = Point(0, 0)
  val b = Point(3, 4)
  println(f"distance = ${a.distanceTo(b)}%.2f")

  val nums = List(1, 2, 3, 4, 5)
  println(s"sum = ${nums.sum}")
  println(s"squares = ${nums.map(_.squared).mkString(", ")}")
  println(s"evens = ${nums.filter(_ % 2 == 0)}")

  for shape <- List(Shape.Circle(1), Shape.Rect(2, 3)) do
    println(f"$shape area = ${area(shape)}%.2f")

// A small Dart tour: a class with a named constructor, a sealed class with a
// switch expression, records, and collection-for. `dart run` runs a lone
// file directly.
import 'dart:math' as math;

class Point {
  final double x, y;
  const Point(this.x, this.y);
  const Point.origin() : this(0, 0);

  double distanceTo(Point other) =>
      math.sqrt(math.pow(x - other.x, 2) + math.pow(y - other.y, 2));

  @override
  String toString() => '($x, $y)';
}

sealed class Shape {}

class Circle extends Shape {
  final double r;
  Circle(this.r);
}

class Rect extends Shape {
  final double w, h;
  Rect(this.w, this.h);
}

double area(Shape shape) => switch (shape) {
      Circle(:final r) => math.pi * r * r,
      Rect(:final w, :final h) => w * h,
    };

(int, int) minMax(List<int> values) =>
    (values.reduce(math.min), values.reduce(math.max));

void main() {
  print('hello, world');

  const a = Point.origin();
  const b = Point(3, 4);
  print('distance $a -> $b = ${a.distanceTo(b).toStringAsFixed(2)}');

  final nums = [for (var i = 1; i <= 5; i++) i];
  print('sum = ${nums.reduce((x, y) => x + y)}');
  print('squares = ${nums.map((n) => n * n).join(', ')}');

  final (lo, hi) = minMax(nums);
  print('min = $lo, max = $hi');

  for (final shape in [Circle(1), Rect(2, 3)]) {
    print('${shape.runtimeType} area = ${area(shape).toStringAsFixed(2)}');
  }
}

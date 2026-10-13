// A small Java tour: a record, a sealed interface with pattern matching,
// and streams. Java 11+ runs a single source file directly (`java File.java`),
// no separate javac step or matching class/file name needed.
import java.util.List;
import java.util.stream.Collectors;
import java.util.stream.IntStream;

public class java_example {
    record Point(double x, double y) {
        double distanceTo(Point other) {
            return Math.hypot(x - other.x, y - other.y);
        }
    }

    sealed interface Shape permits Circle, Rect {}
    record Circle(double r) implements Shape {}
    record Rect(double w, double h) implements Shape {}

    static double area(Shape shape) {
        return switch (shape) {
            case Circle c -> Math.PI * c.r() * c.r();
            case Rect r -> r.w() * r.h();
        };
    }

    public static void main(String[] args) {
        System.out.println("hello, world");

        Point a = new Point(0, 0);
        Point b = new Point(3, 4);
        System.out.printf("distance = %.2f%n", a.distanceTo(b));

        List<Integer> nums = IntStream.rangeClosed(1, 5).boxed().toList();
        System.out.println("sum = " + nums.stream().mapToInt(Integer::intValue).sum());
        System.out.println("squares = " + nums.stream().map(n -> n * n).collect(Collectors.toList()));

        for (Shape s : List.of(new Circle(1), new Rect(2, 3))) {
            System.out.printf("%s area = %.2f%n", s.getClass().getSimpleName(), area(s));
        }
    }
}

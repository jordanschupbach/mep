<?php
// A small PHP tour: a readonly class with constructor promotion, a match
// expression, arrow functions, and array helpers. Runs under the php CLI.

final class Point
{
    public function __construct(
        public readonly float $x,
        public readonly float $y,
    ) {}

    public function distanceTo(Point $other): float
    {
        return hypot($this->x - $other->x, $this->y - $other->y);
    }
}

function area(array $shape): float
{
    return match ($shape['kind']) {
        'circle' => M_PI * $shape['r'] ** 2,
        'rect' => $shape['w'] * $shape['h'],
    };
}

echo "hello, world\n";

$a = new Point(0, 0);
$b = new Point(3, 4);
printf("distance = %.2f\n", $a->distanceTo($b));

$nums = range(1, 5);
echo 'sum = ', array_sum($nums), "\n";
echo 'squares = ', implode(', ', array_map(fn ($n) => $n * $n, $nums)), "\n";

foreach ([['kind' => 'circle', 'r' => 1], ['kind' => 'rect', 'w' => 2, 'h' => 3]] as $shape) {
    printf("%s area = %.2f\n", $shape['kind'], area($shape));
}

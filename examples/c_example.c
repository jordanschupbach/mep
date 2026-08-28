#include <stdio.h>
#include <math.h>

typedef struct {
    double x;
    double y;
} Point;

double distance(Point a, Point b) {
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
}

int main(void) {
    printf("hello, world\n");

    Point a = {0.0, 0.0};
    Point b = {3.0, 4.0};
    printf("distance = %.2f\n", distance(a, b));

    return 0;
}


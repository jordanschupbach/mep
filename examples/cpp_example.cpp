#include <iostream>
#include <vector>
#include <cmath>

class Point {
public:
    Point(double x, double y) : x_(x), y_(y) {}

    double distanceTo(const Point& other) const {
        double dx = x_ - other.x_;
        double dy = y_ - other.y_;
        return std::sqrt(dx * dx + dy * dy);
    }

private:
    double x_;
    double y_;
};

template <typename T>
T sum(const std::vector<T>& values) {
    T total{};
    for (const auto& v : values) {
        total += v;
    }
    return total;
}

int main() {
    std::cout << "hello, world" << std::endl;

    Point a(0.0, 0.0);
    Point b(3.0, 4.0);
    std::cout << "distance = " << a.distanceTo(b) << std::endl;

    std::vector<int> nums{1, 2, 3, 4, 5};
    std::cout << "sum = " << sum(nums) << std::endl;

    return 0;
}

// A small Go tour: a struct with a method, an interface, a generic function,
// and a goroutine fan-in over a channel. `go run` builds and runs it.
package main

import (
	"fmt"
	"math"
	"sort"
)

type Point struct {
	X, Y float64
}

func (p Point) DistanceTo(o Point) float64 {
	return math.Hypot(p.X-o.X, p.Y-o.Y)
}

type Shape interface {
	Area() float64
}

type Circle struct{ R float64 }
type Rect struct{ W, H float64 }

func (c Circle) Area() float64 { return math.Pi * c.R * c.R }
func (r Rect) Area() float64   { return r.W * r.H }

func Sum[T int | float64](values []T) T {
	var total T
	for _, v := range values {
		total += v
	}
	return total
}

func main() {
	fmt.Println("hello, world")

	a, b := Point{0, 0}, Point{3, 4}
	fmt.Printf("distance = %.2f\n", a.DistanceTo(b))

	nums := []int{1, 2, 3, 4, 5}
	fmt.Println("sum =", Sum(nums))

	squares := make(chan int)
	for _, n := range nums {
		go func(n int) { squares <- n * n }(n)
	}
	got := make([]int, 0, len(nums))
	for range nums {
		got = append(got, <-squares)
	}
	sort.Ints(got)
	fmt.Println("squares =", got)

	for _, s := range []Shape{Circle{1}, Rect{2, 3}} {
		fmt.Printf("%T area = %.2f\n", s, s.Area())
	}
}

// A small Rust tour: a struct with impl, a trait, an enum with match, and
// iterator adaptors. Compiled with rustc, then run.
use std::collections::BTreeMap;
use std::fmt;

#[derive(Debug, Clone, Copy)]
struct Point {
    x: f64,
    y: f64,
}

impl Point {
    fn distance_to(&self, other: &Point) -> f64 {
        (self.x - other.x).hypot(self.y - other.y)
    }
}

impl fmt::Display for Point {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "({}, {})", self.x, self.y)
    }
}

enum Shape {
    Circle(f64),
    Rect { w: f64, h: f64 },
}

impl Shape {
    fn area(&self) -> f64 {
        match self {
            Shape::Circle(r) => std::f64::consts::PI * r * r,
            Shape::Rect { w, h } => w * h,
        }
    }
}

fn main() {
    println!("hello, world");

    let a = Point { x: 0.0, y: 0.0 };
    let b = Point { x: 3.0, y: 4.0 };
    println!("distance {a} -> {b} = {:.2}", a.distance_to(&b));

    let nums = vec![1, 2, 3, 4, 5];
    println!("sum = {}", nums.iter().sum::<i32>());
    let squares: Vec<i32> = nums.iter().map(|n| n * n).collect();
    println!("squares = {squares:?}");

    for shape in [Shape::Circle(1.0), Shape::Rect { w: 2.0, h: 3.0 }] {
        println!("area = {:.2}", shape.area());
    }

    let mut by_letter = BTreeMap::new();
    for word in ["apple", "banana", "avocado", "cherry"] {
        *by_letter.entry(word.chars().next().unwrap()).or_insert(0) += 1;
    }
    println!("by first letter = {by_letter:?}");
}

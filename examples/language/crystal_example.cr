# A small Crystal tour: a struct, type annotations, blocks, and a generic
# method. Ruby-like syntax, compiled -- `crystal run` builds and runs it.

struct Point
  getter x : Float64
  getter y : Float64

  def initialize(@x : Float64, @y : Float64)
  end

  def distance_to(other : Point) : Float64
    Math.hypot(x - other.x, y - other.y)
  end

  def to_s(io : IO) : Nil
    io << "(" << x << ", " << y << ")"
  end
end

def sum(values : Array(T)) : T forall T
  values.sum
end

puts "hello, world"

a = Point.new(0.0, 0.0)
b = Point.new(3.0, 4.0)
puts "distance #{a} -> #{b} = #{a.distance_to(b).round(2)}"

nums = [1, 2, 3, 4, 5]
puts "sum = #{sum(nums)}"
puts "squares = #{nums.map { |n| n * n }.join(", ")}"
puts "evens = #{nums.select(&.even?)}"

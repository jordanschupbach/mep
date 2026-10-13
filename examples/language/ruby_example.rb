# A small Ruby tour: a Struct with a method, blocks, and enumerable chains.

Point = Struct.new(:x, :y) do
  def distance_to(other)
    Math.hypot(x - other.x, y - other.y)
  end

  def to_s
    "(#{x}, #{y})"
  end
end

def sum(values)
  values.sum
end

puts "hello, world"

a = Point.new(0.0, 0.0)
b = Point.new(3.0, 4.0)
puts format("distance %s -> %s = %.2f", a, b, a.distance_to(b))

nums = [1, 2, 3, 4, 5]
puts "sum = #{sum(nums)}"
puts "squares = #{nums.map { |n| n * n }.join(', ')}"
puts "evens = #{nums.select(&:even?)}"

counts = %w[apple banana avocado cherry].group_by { |w| w[0] }.transform_values(&:size)
puts "by first letter = #{counts}"

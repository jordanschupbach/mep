# A small Elixir tour: a struct, pattern matching, the pipe operator, and
# Enum. A .exs script, so `elixir` runs it without a Mix project.

defmodule Point do
  defstruct x: 0, y: 0

  # A script can't use %Point{} at its own top level (the struct isn't
  # compiled yet there), so build points through this instead.
  def new(x, y), do: %Point{x: x, y: y}

  def distance(%Point{x: x1, y: y1}, %Point{x: x2, y: y2}) do
    :math.sqrt(:math.pow(x1 - x2, 2) + :math.pow(y1 - y2, 2))
  end
end

defmodule Shapes do
  def area({:circle, r}), do: :math.pi() * r * r
  def area({:rect, w, h}), do: w * h
end

IO.puts("hello, world")

a = Point.new(0, 0)
b = Point.new(3, 4)
IO.puts("distance = #{:erlang.float_to_binary(Point.distance(a, b), decimals: 2)}")

nums = [1, 2, 3, 4, 5]
IO.puts("sum = #{Enum.sum(nums)}")
IO.puts("squares = #{nums |> Enum.map(&(&1 * &1)) |> Enum.join(", ")}")
IO.puts("evens = #{inspect(Enum.filter(nums, &(rem(&1, 2) == 0)))}")

for shape <- [{:circle, 1}, {:rect, 2, 3}] do
  IO.puts("#{elem(shape, 0)} area = #{Float.round(Shapes.area(shape) / 1, 2)}")
end

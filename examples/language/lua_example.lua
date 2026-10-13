-- A small Lua tour: tables as records, closures, varargs, and metatables.

local Point = {}
Point.__index = Point

function Point.new(x, y)
    return setmetatable({ x = x, y = y }, Point)
end

function Point:distance_to(other)
    local dx, dy = self.x - other.x, self.y - other.y
    return math.sqrt(dx * dx + dy * dy)
end

Point.__tostring = function(p)
    return string.format("(%g, %g)", p.x, p.y)
end

local function sum(...)
    local total = 0
    for _, v in ipairs({ ... }) do
        total = total + v
    end
    return total
end

local function counter()
    local n = 0
    return function()
        n = n + 1
        return n
    end
end

print("hello, world")

local a, b = Point.new(0, 0), Point.new(3, 4)
print(string.format("distance %s -> %s = %.2f", tostring(a), tostring(b), a:distance_to(b)))
print("sum = " .. sum(1, 2, 3, 4, 5))

local next_id = counter()
next_id()
print("counter = " .. next_id())

local squares = {}
for i = 1, 5 do
    squares[#squares + 1] = i * i
end
print("squares = " .. table.concat(squares, ", "))

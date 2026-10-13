// A small Zig tour: a struct with a method, a tagged union with switch, a
// comptime-generic function, and slices. `zig run` builds and runs it.
const std = @import("std");

const Point = struct {
    x: f64,
    y: f64,

    fn distanceTo(self: Point, other: Point) f64 {
        return std.math.hypot(self.x - other.x, self.y - other.y);
    }
};

const Shape = union(enum) {
    circle: f64,
    rect: struct { w: f64, h: f64 },

    fn area(self: Shape) f64 {
        return switch (self) {
            .circle => |r| std.math.pi * r * r,
            .rect => |r| r.w * r.h,
        };
    }
};

fn sum(comptime T: type, values: []const T) T {
    var total: T = 0;
    for (values) |v| total += v;
    return total;
}

pub fn main() void {
    std.debug.print("hello, world\n", .{});

    const a = Point{ .x = 0, .y = 0 };
    const b = Point{ .x = 3, .y = 4 };
    std.debug.print("distance = {d:.2}\n", .{a.distanceTo(b)});

    const nums = [_]i32{ 1, 2, 3, 4, 5 };
    std.debug.print("sum = {d}\n", .{sum(i32, &nums)});

    std.debug.print("squares =", .{});
    for (nums) |n| std.debug.print(" {d}", .{n * n});
    std.debug.print("\n", .{});

    const shapes = [_]Shape{ .{ .circle = 1 }, .{ .rect = .{ .w = 2, .h = 3 } } };
    for (shapes) |s| std.debug.print("{s} area = {d:.2}\n", .{ @tagName(s), s.area() });
}

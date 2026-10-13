// A small TypeScript tour: an interface, a generic function, a union type,
// and a type guard. Runs under bun (no separate tsc step).

interface Point {
  x: number;
  y: number;
}

function distance(a: Point, b: Point): number {
  return Math.hypot(a.x - b.x, a.y - b.y);
}

function sum<T extends number>(values: readonly T[]): number {
  return values.reduce((total, v) => total + v, 0);
}

type Shape =
  | { kind: "circle"; radius: number }
  | { kind: "rect"; width: number; height: number };

function area(shape: Shape): number {
  switch (shape.kind) {
    case "circle":
      return Math.PI * shape.radius ** 2;
    case "rect":
      return shape.width * shape.height;
  }
}

console.log("hello, world");

const a: Point = { x: 0, y: 0 };
const b: Point = { x: 3, y: 4 };
console.log(`distance = ${distance(a, b).toFixed(2)}`);

const nums = [1, 2, 3, 4, 5] as const;
console.log(`sum = ${sum(nums)}`);
console.log(`squares = ${nums.map((n) => n * n).join(", ")}`);

const shapes: Shape[] = [
  { kind: "circle", radius: 1 },
  { kind: "rect", width: 2, height: 3 },
];
for (const s of shapes) {
  console.log(`${s.kind} area = ${area(s).toFixed(2)}`);
}

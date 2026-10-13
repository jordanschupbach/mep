// A small JavaScript tour: a class, arrow functions, destructuring, and
// array methods. Runs under node.

class Point {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }

  distanceTo({ x, y }) {
    return Math.hypot(this.x - x, this.y - y);
  }

  toString() {
    return `(${this.x}, ${this.y})`;
  }
}

const sum = (values) => values.reduce((total, v) => total + v, 0);

console.log("hello, world");

const a = new Point(0, 0);
const b = new Point(3, 4);
console.log(`distance ${a} -> ${b} = ${a.distanceTo(b).toFixed(2)}`);

const nums = [1, 2, 3, 4, 5];
console.log(`sum = ${sum(nums)}`);
console.log(`squares = ${nums.map((n) => n * n).join(", ")}`);
console.log(`evens = ${JSON.stringify(nums.filter((n) => n % 2 === 0))}`);

const counts = {};
for (const word of ["apple", "banana", "avocado", "cherry"]) {
  counts[word[0]] = (counts[word[0]] ?? 0) + 1;
}
console.log("by first letter =", counts);

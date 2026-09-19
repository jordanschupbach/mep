export { PI } from './constants.js';
export function area(shape) {
  if (shape.kind === 'square') return shape.side * shape.side;
  return 0;
}

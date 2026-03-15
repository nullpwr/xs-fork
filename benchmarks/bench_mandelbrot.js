"use strict";
function mandel(cx, cy, max) {
  let zx = 0, zy = 0;
  for (let i = 0; i < max; i++) {
    if (zx * zx + zy * zy > 4) return i;
    const tx = zx * zx - zy * zy + cx;
    zy = 2 * zx * zy + cy;
    zx = tx;
  }
  return max;
}
const size = 200, max = 500;
let total = 0;
for (let py = 0; py < size; py++) {
  for (let px = 0; px < size; px++) {
    total += mandel(-2 + 3 * (px / size), -1.5 + 3 * (py / size), max);
  }
}
console.log("mandelbrot checksum =", total);

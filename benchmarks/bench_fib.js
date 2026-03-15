"use strict";
function fib(n) {
  if (n <= 1) return n;
  return fib(n - 1) + fib(n - 2);
}
const r = fib(30);
if (r !== 832040) throw new Error("mismatch");
console.log("fib(30) =", r);

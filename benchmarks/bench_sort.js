"use strict";
function quicksort(arr) {
  if (arr.length <= 1) return arr;
  const pivot = arr[0], left = [], right = [];
  for (let i = 1; i < arr.length; i++) {
    (arr[i] < pivot ? left : right).push(arr[i]);
  }
  return [...quicksort(left), pivot, ...quicksort(right)];
}

const data = [];
let seed = 42;
for (let i = 0; i < 1000; i++) {
  seed = (seed * 1103515245 + 12345) % (2 ** 31);
  data.push(seed % 10000);
}
const s = quicksort(data);
if (s.length !== 1000) throw new Error("length");
for (let j = 1; j < s.length; j++) if (s[j - 1] > s[j]) throw new Error("order");
console.log("sorted", s.length, "values");

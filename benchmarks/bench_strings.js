"use strict";
let result = "";
for (let i = 0; i < 500; i++) result = result + String(i) + " ";
if (result.length === 0) throw new Error("empty");

const words = result.trim().split(" ");
if (words.length !== 500) throw new Error("count");

let upper_count = 0;
for (const w of words) {
  if (w.toUpperCase() === w) upper_count++;
}
console.log("processed", words.length, "words");

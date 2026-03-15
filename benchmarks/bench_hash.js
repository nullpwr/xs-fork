"use strict";
const crypto = require("crypto");
const buf = "the quick brown fox jumps over the lazy dog 0123456789 ".repeat(1000);
const total = 50;
let last = "";
for (let i = 0; i < total; i++) last = crypto.createHash("sha256").update(buf).digest("hex");
if (last.length !== 64) throw new Error("len");
console.log("hash rounds =", total, "digest =", last);

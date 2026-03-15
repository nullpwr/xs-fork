"use strict";
const records = [];
for (let i = 0; i < 500; i++) {
  records.push({
    id: i, name: `user_${i}`, email: `user${i}@example.com`,
    active: i % 3 === 0, tags: ["a", "b", "c"],
  });
}
const s = JSON.stringify(records);
const parsed = JSON.parse(s);
if (parsed.length !== 500) throw new Error("length");
const s2 = JSON.stringify(parsed);
if (s.length !== s2.length) throw new Error("roundtrip");
console.log("json records =", parsed.length);

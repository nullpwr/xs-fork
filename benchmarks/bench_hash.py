import hashlib

buf = "the quick brown fox jumps over the lazy dog 0123456789 " * 1000
total = 50
last = ""
for _ in range(total):
    last = hashlib.sha256(buf.encode()).hexdigest()

assert len(last) == 64
print(f"hash rounds = {total} digest = {last}")

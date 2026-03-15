result = ""
for i in range(500):
    result = result + str(i) + " "
assert len(result) > 0

words = result.strip().split(" ")
assert len(words) == 500

upper_count = 0
for w in words:
    u = w.upper()
    if u == w:
        upper_count += 1

print(f"processed {len(words)} words")

def quicksort(arr):
    if len(arr) <= 1:
        return arr
    pivot = arr[0]
    left, right = [], []
    for x in arr[1:]:
        (left if x < pivot else right).append(x)
    return quicksort(left) + [pivot] + quicksort(right)

data = []
seed = 42
for _ in range(1000):
    seed = (seed * 1103515245 + 12345) % (2 ** 31)
    data.append(seed % 10000)

sorted_data = quicksort(data)
assert len(sorted_data) == 1000
for j in range(1, 1000):
    assert sorted_data[j - 1] <= sorted_data[j]

print(f"sorted {len(sorted_data)} values")

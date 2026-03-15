import json

records = [
    {
        "id": i,
        "name": f"user_{i}",
        "email": f"user{i}@example.com",
        "active": i % 3 == 0,
        "tags": ["a", "b", "c"],
    }
    for i in range(500)
]

s = json.dumps(records)
parsed = json.loads(s)
assert len(parsed) == 500

s2 = json.dumps(parsed)
assert len(s) == len(s2)
print(f"json records = {len(parsed)}")

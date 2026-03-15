def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

r = fib(30)
assert r == 832040
print(f"fib(30) = {r}")

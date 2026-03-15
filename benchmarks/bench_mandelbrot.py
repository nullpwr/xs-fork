def mandel(cx, cy, max_iter):
    zx = 0.0
    zy = 0.0
    for i in range(max_iter):
        if zx * zx + zy * zy > 4.0:
            return i
        tx = zx * zx - zy * zy + cx
        zy = 2.0 * zx * zy + cy
        zx = tx
    return max_iter

size = 200
max_iter = 500
total = 0
for py in range(size):
    for px in range(size):
        cx = -2.0 + 3.0 * (px / size)
        cy = -1.5 + 3.0 * (py / size)
        total += mandel(cx, cy, max_iter)

print(f"mandelbrot checksum = {total}")

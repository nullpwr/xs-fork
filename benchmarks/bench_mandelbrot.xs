-- mandelbrot: count iterations per pixel, sum checksum
-- 200x200 grid, max 500 iterations

fn mandel(cx, cy, max) {
    var zx = 0.0
    var zy = 0.0
    var i = 0
    while i < max {
        if zx * zx + zy * zy > 4.0 { return i }
        let tx = zx * zx - zy * zy + cx
        zy = 2.0 * zx * zy + cy
        zx = tx
        i = i + 1
    }
    return max
}

var total = 0
let size = 200
let max_iter = 500
let fsize = 200.0
var py = 0
while py < size {
    var px = 0
    while px < size {
        let cx = -2.0 + 3.0 * (float(px) / fsize)
        let cy = -1.5 + 3.0 * (float(py) / fsize)
        total = total + mandel(cx, cy, max_iter)
        px = px + 1
    }
    py = py + 1
}

println("mandelbrot checksum =", total)

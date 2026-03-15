package main

import "fmt"

func mandel(cx, cy float64, max int) int {
	zx, zy := 0.0, 0.0
	for i := 0; i < max; i++ {
		if zx*zx+zy*zy > 4.0 {
			return i
		}
		tx := zx*zx - zy*zy + cx
		zy = 2.0*zx*zy + cy
		zx = tx
	}
	return max
}

func main() {
	size := 200
	max := 500
	total := 0
	for py := 0; py < size; py++ {
		for px := 0; px < size; px++ {
			total += mandel(-2.0+3.0*(float64(px)/float64(size)), -1.5+3.0*(float64(py)/float64(size)), max)
		}
	}
	fmt.Println("mandelbrot checksum =", total)
}

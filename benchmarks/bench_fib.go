package main

import "fmt"

func fib(n int) int {
	if n <= 1 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	r := fib(30)
	if r != 832040 {
		panic("mismatch")
	}
	fmt.Println("fib(30) =", r)
}

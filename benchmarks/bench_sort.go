package main

import "fmt"

func quicksort(arr []int) []int {
	if len(arr) <= 1 {
		r := make([]int, len(arr))
		copy(r, arr)
		return r
	}
	pivot := arr[0]
	var left, right []int
	for _, x := range arr[1:] {
		if x < pivot {
			left = append(left, x)
		} else {
			right = append(right, x)
		}
	}
	l := quicksort(left)
	r := quicksort(right)
	out := make([]int, 0, len(arr))
	out = append(out, l...)
	out = append(out, pivot)
	out = append(out, r...)
	return out
}

func main() {
	data := make([]int, 0, 1000)
	seed := uint64(42)
	for i := 0; i < 1000; i++ {
		seed = (seed*1103515245 + 12345) % (1 << 31)
		data = append(data, int(seed%10000))
	}
	s := quicksort(data)
	if len(s) != 1000 {
		panic("length")
	}
	for j := 1; j < len(s); j++ {
		if s[j-1] > s[j] {
			panic("order")
		}
	}
	fmt.Println("sorted", len(s), "values")
}

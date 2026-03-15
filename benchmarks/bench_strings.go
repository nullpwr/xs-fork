package main

import (
	"fmt"
	"strconv"
	"strings"
)

func main() {
	result := ""
	for i := 0; i < 500; i++ {
		result = result + strconv.Itoa(i) + " "
	}
	if len(result) == 0 {
		panic("empty")
	}
	words := strings.Split(strings.TrimSpace(result), " ")
	if len(words) != 500 {
		panic("count")
	}
	upperCount := 0
	for _, w := range words {
		if strings.ToUpper(w) == w {
			upperCount++
		}
	}
	fmt.Println("processed", len(words), "words")
}

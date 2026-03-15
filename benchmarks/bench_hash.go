package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"strings"
)

func main() {
	buf := strings.Repeat("the quick brown fox jumps over the lazy dog 0123456789 ", 1000)
	total := 50
	last := ""
	for i := 0; i < total; i++ {
		h := sha256.Sum256([]byte(buf))
		last = hex.EncodeToString(h[:])
	}
	if len(last) != 64 {
		panic("len")
	}
	fmt.Println("hash rounds =", total, "digest =", last)
}

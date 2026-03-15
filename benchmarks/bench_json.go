package main

import (
	"encoding/json"
	"fmt"
)

type Rec struct {
	ID     int      `json:"id"`
	Name   string   `json:"name"`
	Email  string   `json:"email"`
	Active bool     `json:"active"`
	Tags   []string `json:"tags"`
}

func main() {
	records := make([]Rec, 500)
	for i := 0; i < 500; i++ {
		records[i] = Rec{
			ID: i, Name: fmt.Sprintf("user_%d", i), Email: fmt.Sprintf("user%d@example.com", i),
			Active: i%3 == 0, Tags: []string{"a", "b", "c"},
		}
	}
	s, err := json.Marshal(records)
	if err != nil {
		panic(err)
	}
	var parsed []Rec
	if err := json.Unmarshal(s, &parsed); err != nil {
		panic(err)
	}
	if len(parsed) != 500 {
		panic("length")
	}
	s2, _ := json.Marshal(parsed)
	if len(s) != len(s2) {
		panic("roundtrip")
	}
	fmt.Println("json records =", len(parsed))
}

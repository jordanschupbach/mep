// go_check: the Go oracle for `just grammar-diff go --oracle`. Parses each
// file with go/parser (the parser behind gofmt and go vet; nothing is type
// checked or run) and exits 1 if any has a syntax error, printing them as
// gofmt -e does. Build it with `just go-check` (into build/grammar-diff/go).
package main

import (
	"fmt"
	"go/parser"
	"go/scanner"
	"go/token"
	"os"
)

func main() {
	status := 0
	for _, path := range os.Args[1:] {
		src, err := os.ReadFile(path)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			status = 2
			continue
		}
		fset := token.NewFileSet()
		_, err = parser.ParseFile(fset, path, src, parser.ParseComments|parser.AllErrors|parser.SkipObjectResolution)
		if err != nil {
			if list, ok := err.(scanner.ErrorList); ok {
				for _, e := range list {
					fmt.Fprintln(os.Stderr, e)
				}
			} else {
				fmt.Fprintln(os.Stderr, err)
			}
			status = 1
		}
	}
	os.Exit(status)
}

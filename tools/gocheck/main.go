// gocheck decodes the streams `fxtest --export DIR` writes with
// golang.org/x/image/ccitt -- a decoder that shares no code with this repo --
// and compares each with the page the plugin scanned.
//
//	go run . DIR
//
// cardN.g3  one-dimensional (MH) coding, an EOL before every line, RTC:
//           the Group 3 stream of ITU-T T.4 4.1, without fill.
// cardN.g4  every line two-dimensional against the one above, no EOLs, EOFB:
//           ITU-T T.6, whose 2-D coding is T.4's MR 2-D coding (4.2).
// cardN.pbm the page, P4, 1 = black.
//
// Exit 1 on any difference.
package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"golang.org/x/image/ccitt"
)

func readPBM(path string) (int, int, []byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return 0, 0, nil, err
	}
	defer f.Close()
	r := bufio.NewReader(f)
	var w, h int
	if _, err := fmt.Fscanf(r, "P4\n%d %d\n", &w, &h); err != nil {
		return 0, 0, nil, err
	}
	data, err := io.ReadAll(r)
	return w, h, data, err
}

func main() {
	if len(os.Args) != 2 {
		fmt.Println("usage: gocheck DIR")
		os.Exit(2)
	}
	dir := os.Args[1]
	pages, _ := filepath.Glob(filepath.Join(dir, "card*.pbm"))
	if len(pages) == 0 {
		fmt.Println("no pages in", dir)
		os.Exit(1)
	}
	failures := 0
	for _, pbm := range pages {
		w, h, want, err := readPBM(pbm)
		if err != nil {
			fmt.Println(pbm, err)
			os.Exit(1)
		}
		stem := pbm[:len(pbm)-4]
		for _, kind := range []struct {
			ext string
			sf  ccitt.SubFormat
		}{{".g3", ccitt.Group3}, {".g4", ccitt.Group4}} {
			stream, err := os.ReadFile(stem + kind.ext)
			if err != nil {
				fmt.Println(err)
				os.Exit(1)
			}
			got, err := io.ReadAll(ccitt.NewReader(bytes.NewReader(stream), ccitt.MSB, kind.sf, w, h, &ccitt.Options{Invert: true}))
			ok := err == nil && bytes.Equal(got, want)
			if !ok {
				failures++
			}
			diff := 0
			for i := range want {
				if i >= len(got) || got[i] != want[i] {
					diff++
				}
			}
			verdict := "ok"
			if !ok {
				verdict = "FAILED"
			}
			fmt.Printf("%s%s: %dx%d, %d bytes of stream, golang.org/x/image/ccitt decodes it to %d of %d page bytes, %d differ (err %v)  %s\n",
				filepath.Base(stem), kind.ext, w, h, len(stream), len(got), len(want), diff, err, verdict)
		}
	}
	if failures > 0 {
		os.Exit(1)
	}
	fmt.Println("gocheck: an independent T.4 / T.6 decoder reads the plugin's coding exactly")
}

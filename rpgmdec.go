package main

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

var rpgmvpHeader = []byte{
	0x52, 0x50, 0x47, 0x4D, 0x56, 0x00, 0x00, 0x00,
	0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
}

var pngHeader = []byte{
	// PNG magic
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
	// Chunk length: 13, this is always fixed
	0x00, 0x00, 0x00, 0x0D,
	// Chunk type: "IHDR"
	0x49, 0x48, 0x44, 0x52,
}

func decrypt(name string) error {
	file, err := os.ReadFile(name)
	if err != nil {
		return err
	}

	if len(file) < len(rpgmvpHeader)+len(pngHeader) {
		return fmt.Errorf("file is too small")
	}
	for i := 0; i < len(rpgmvpHeader); i++ {
		if file[i] != rpgmvpHeader[i] {
			return fmt.Errorf("invalid magic")
		}
	}

	file = file[len(rpgmvpHeader):]
	for i := 0; i < len(pngHeader); i++ {
		file[i] = pngHeader[i]
	}

	ext := filepath.Ext(name)
	newName := name[:len(name)-len(ext)] + ".png"

	if err = os.WriteFile(newName, file, 0o644); err != nil {
		return fmt.Errorf("write decrypted file: %w", err)
	}
	return nil
}

func decryptWorker(wg *sync.WaitGroup, jobs chan string) {
	defer wg.Done()
	for name := range jobs {
		if err := decrypt(name); err != nil {
			log.Printf("failed to decrypt %q: %v\n", name, err)
		}
	}
}

func DecryptDir(name string) error {
	entries, err := os.ReadDir(name)
	if err != nil {
		return err
	}

	jobs := make(chan string)
	wg := &sync.WaitGroup{}
	numWorkers := runtime.GOMAXPROCS(0)
	for i := 0; i < numWorkers; i++ {
		wg.Add(1)
		go decryptWorker(wg, jobs)
	}

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}

		baseName := entry.Name()
		if !strings.HasSuffix(baseName, ".rpgmvp") {
			continue
		}

		jobs <- filepath.Join(name, baseName)
	}

	close(jobs)
	wg.Wait()

	return nil
}

func main() {
	if len(os.Args) == 1 {
		log.Fatal("directory required")
	}
	if err := DecryptDir(os.Args[1]); err != nil {
		log.Fatal(err)
	}
}

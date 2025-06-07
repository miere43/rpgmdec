package main

import (
	"fmt"
	"io/fs"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"
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

var jobsCreated atomic.Int32
var jobsFinished atomic.Int32

func decrypt(name string) error {
	file, err := os.ReadFile(name)
	if err != nil {
		return err
	}

	if len(file) < len(rpgmvpHeader)+len(pngHeader) {
		return fmt.Errorf("file is too small")
	}
	for i := range rpgmvpHeader {
		if file[i] != rpgmvpHeader[i] {
			return fmt.Errorf("invalid magic")
		}
	}

	file = file[len(rpgmvpHeader):]
	copy(file, pngHeader)

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
		jobsFinished.Add(1)
	}
}

func printProgressWorker(wg *sync.WaitGroup, done <-chan struct{}) {
	defer wg.Done()

	started := time.Now()

	for {
		select {
		case <-done:
			fmt.Printf("Processed %d/%d files...\n", jobsFinished.Load(), jobsCreated.Load())
			fmt.Printf("Done in %v\n", time.Now().Sub(started))
			return
		case <-time.After(time.Millisecond * 300):
			fmt.Printf("Processed %d/%d files...\n", jobsFinished.Load(), jobsCreated.Load())
		}
	}
}

func DecryptDir(name string) error {
	jobs := make(chan string)
	wg := &sync.WaitGroup{}
	numWorkers := runtime.GOMAXPROCS(0)
	for range numWorkers {
		wg.Add(1)
		go decryptWorker(wg, jobs)
	}

	printWG := &sync.WaitGroup{}
	printWG.Add(1)
	printDone := make(chan struct{})
	go printProgressWorker(printWG, printDone)

	err := filepath.WalkDir(name, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !entry.IsDir() && (strings.HasSuffix(path, ".rpgmvp") || strings.HasSuffix(path, ".png_")) {
			jobsCreated.Add(1)
			jobs <- path
		}
		return nil
	})
	if err != nil {
		return err
	}

	close(jobs)
	wg.Wait()

	close(printDone)
	printWG.Wait()

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

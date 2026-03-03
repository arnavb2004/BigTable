# BigTable

A high-performance, distributed key-value store inspired by [Google's Bigtable paper](https://static.googleusercontent.com/media/research.google.com/en//archive/bigtable-osdi06.pdf).

## Current Progress: Arena Allocator

The project currently features a custom **Arena Allocator** designed to handle high-frequency, small-object allocations with minimal overhead.

### Key Features

* **Efficient Memory Management:** Minimizes heap fragmentation by allocating memory in 4KB blocks.
* **Alignment Guarantees:** Ensures all allocations are 8-byte aligned for optimal CPU performance.
* **Thread-Safe Tracking:** Uses atomic counters to track memory usage in real-time.

### How to Run

To compile and run the test suite, ensure you have `make` (or `mingw32-make`) installed:

```bash
# Compile and run the tests
make

# Clean up build artifacts
make clean
```
If you do not have `make` installed, you can use the following command:

```bash
g++ -std=c++17 -Wall -c arena.cpp -o arena.o; g++ -std=c++17 -Wall arena_test.cpp arena.o -o arena_test; .\arena_test
```
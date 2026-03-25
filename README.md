# BigTable

A from-scratch C++ implementation of
[Google's Bigtable paper](https://static.googleusercontent.com/media/research.google.com/en//archive/bigtable-osdi06.pdf)
— a distributed storage system for structured data designed to scale to
petabytes across thousands of commodity servers.

---

## Project structure

```
BigTable/
├── Makefile                  ← root orchestrator (build + test everything)
├── ArenaAllocator/
│   ├── arena.hpp
│   ├── arena.cpp
│   ├── arena_test.cpp
│   └── Makefile
|── SkipList/
|    ├── skiplist.hpp
|    ├── skiplist.ipp           ← template implementation (#included by .hpp)
|    ├── skiplist_test.cpp
|    └── Makefile
└── Readme.md
```

---

## Components

### 1. Arena allocator

A slab-based memory manager designed for high-frequency, small-object
allocations with minimal overhead — the same strategy used by LevelDB and
RocksDB for their MemTable nodes.

| Property | Detail |
|---|---|
| Block size | 4 KB (one OS page) |
| Allocation strategy | Bump-pointer — O(1) per allocation |
| Alignment | 8-byte guaranteed (`AllocateAligned`) |
| Memory tracking | `std::atomic<size_t>` counter — no mutex needed |
| Huge objects | Objects > 1 KB get a dedicated block |

Nodes that don't fit in the current block trigger a fresh 4 KB allocation;
objects larger than 1/4 of the block size get their own dedicated allocation.
The Arena owns all memory and frees it in bulk on destruction — there is no
per-node `delete`.

### 2. SkipList (MemTable backbone)

A probabilistic sorted data structure that serves as the in-memory write
buffer (MemTable) for each tablet. Modelled directly on the LevelDB SkipList.

| Property | Detail |
|---|---|
| Key order | Strictly ascending by comparator |
| Max height | 12 levels |
| Promotion probability | P = 0.25 per level |
| Search complexity | O(log N) expected |
| Write concurrency | Single writer — caller must hold a mutex |
| Read concurrency | Lock-free — multiple concurrent readers are safe |
| Memory | All nodes allocated from an Arena |

**Thread-safety model:** reads are lock-free via `acquire/release` ordering on
the `next[]` pointer array. Writes must be serialised by the caller (e.g. a
per-tablet mutex). This matches the LevelDB MemTable contract and is the
correct model for an LSM-tree write path.

**Composite key support:** the comparator is a template parameter, so the
SkipList natively handles Bigtable-style keys
`(row, column_family:qualifier, timestamp)` where timestamps sort in
descending order so the newest version is always returned first.

---

## Building

### Requirements

- g++ with C++17 support (`-std=c++17`)
- GNU Make (Linux/macOS) or `mingw32-make` (Windows)

### Commands

```bash
# Build everything
make

# Build + run all test suites
make test

# Build a single component
make arena
make skiplist

# Clean all build artifacts
make clean
```

On Windows, replace `make` with `mingw32-make` in all commands above.

### Compiling manually (without Make)

```bash
# Arena tests
cd ArenaAllocator
g++ -std=c++17 -Wall -Wextra -g arena_test.cpp arena.cpp -o arena_test
./arena_test

# SkipList tests
cd SkipList
g++ -std=c++17 -Wall -Wextra -g skiplist_test.cpp ../ArenaAllocator/arena.cpp -o skiplist_test
./skiplist_test        # Linux/macOS
skiplist_test.exe      # Windows (add -lpthread to the command above)
```

---

## Test coverage

| Suite | Tests | What it covers |
|---|---|---|
| Basic | 7 | Insert, search, upsert, missing keys |
| Iterator | 9 | Forward traversal, Seek, rewind |
| Order | 3 | Ascending invariant, no duplicates |
| StringKey | 3 | Lexicographic order, prefix seek |
| MemKey | 4 | Bigtable composite keys, timestamp ordering |
| Scale | 4 | 1K–5K random inserts, ground-truth map comparison |
| Arena | 2 | Memory growth, two lists sharing one arena |
| Boundary | 10 | INT_MIN/MAX, INT64 timestamps, empty string, 10KB keys |
| Concurrency | 5 | 8-thread concurrent reads, seeks, serialised writes |

---

## Roadmap

Implementing the paper bottom-up:

- [x] Arena allocator
- [x] SkipList (MemTable backbone)
- [ ] Memtable — wraps SkipList with a `(row, col, timestamp)` composite key interface
- [ ] SSTable — immutable on-disk sorted file with block index and Bloom filter
- [ ] Commit log — append-only WAL, one per tablet server
- [ ] Tablet — owns one Memtable + a list of SSTables; handles the read/write path
- [ ] Compactor — minor (memtable → SSTable) and major (N SSTables → 1) compaction
- [ ] Tablet server — gRPC server managing N tablets
- [ ] METADATA table — 3-level B+ tree tablet location hierarchy
- [ ] Master server — tablet assignment, load balancing, failure detection

---

## References

- [Bigtable: A Distributed Storage System for Structured Data](https://static.googleusercontent.com/media/research.google.com/en//archive/bigtable-osdi06.pdf) — Chang et al., OSDI 2006
- [LevelDB](https://github.com/google/leveldb) — open-source implementation that shares the same MemTable/SSTable design
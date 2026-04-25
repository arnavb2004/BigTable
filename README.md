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
├── SkipList/
│   ├── skiplist.hpp
│   ├── skiplist.ipp           ← template implementation (#included by .hpp)
│   ├── skiplist_test.cpp
│   └── Makefile
├── MemTable/
│   ├── internal_key.hpp       ← cell identity, sort order, encode/decode
│   ├── internal_key.cpp
│   ├── internal_key_test.cpp
│   ├── memtable.hpp           ← in-memory write buffer
│   ├── memtable.cpp
│   ├── memtable_test.cpp
│   └── Makefile
└── README.md
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

### 3. InternalKey (key schema layer)

Defines the Bigtable cell identity and sort order. Every cell is uniquely
identified by four fields: `(row, col, timestamp, type)`. This layer sits
between the raw SkipList and the Memtable — it knows nothing about either,
and both depend on it.

| Property | Detail |
|---|---|
| Row | Arbitrary byte string (e.g. `"com.google.www"`) |
| Column | `family:qualifier` string (e.g. `"contents:html"`) |
| Timestamp | `int64_t` unix micros — higher value = newer |
| Type | `kTypeValue` (live entry) or `kTypeDeletion` (tombstone) |
| Sort order | row ASC → col ASC → timestamp DESC → type DESC |

**Encode/Decode:** packs all four fields into a single flat binary string for
WAL and SSTable serialisation. Timestamps and type are bitwise-complemented so
that bytewise string comparison of encoded keys agrees exactly with the
in-memory comparator — SSTable binary search requires no custom comparator on
disk.

**Binary layout:**
```
[row_size : 4B big-endian][row][col_size : 4B big-endian][col][~timestamp : 8B big-endian][~type : 1B]
```

### 4. Memtable (in-memory write buffer)

The public write/read interface for a Bigtable tablet. Wraps the SkipList and
Arena behind a clean `Put / Delete / Get` API. Callers never interact with
the SkipList or Arena directly — both are internal implementation details,
swappable without changing the public interface.

| Property | Detail |
|---|---|
| Write path | `Put` / `Delete` → inserts into SkipList |
| Read path | `Get` → seeks to newest version of `(row, col)` |
| Versioning | Every `Put` creates a new version; `Get` always returns newest |
| Tombstones | `Delete` inserts a `kTypeDeletion` marker; pruned at compaction |
| Iterator | Walks all entries in sorted order — used by minor compaction |
| Size tracking | `ApproximateSize()` → Arena usage; Tablet uses this to decide when to flush |
| Thread safety | Same contract as SkipList — writes serialised by caller, reads lock-free |

**Ownership model:** Memtable owns its Arena and SkipList. The underlying data
structure is an implementation detail — replacing the SkipList with a B-tree
or adding a Bloom filter requires no changes to the public interface.

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
make memtable

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
g++ -std=c++17 -Wall -Wextra -g -lpthread skiplist_test.cpp ../ArenaAllocator/arena.cpp -o skiplist_test
./skiplist_test        # Linux/macOS
skiplist_test.exe      # Windows

# InternalKey tests
cd MemTable
g++ -std=c++17 -Wall -Wextra -g internal_key_test.cpp internal_key.cpp -o internal_key_test
./internal_key_test

# Memtable tests
cd MemTable
g++ -std=c++17 -Wall -Wextra -g memtable_test.cpp memtable.cpp internal_key.cpp ../ArenaAllocator/arena.cpp -o memtable_test
./memtable_test
```

---

## Test coverage

### ArenaAllocator

| Suite | Tests | What it covers |
|---|---|---|
| Basic | 5 | Null returns, writability, alignment |
| Memory | 3 | Usage tracking, block accounting |
| Stress | 3 | Many small allocations, aligned stress, mixed |
| Isolation | 1 | Multiple arena instances are independent |

### SkipList

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

### InternalKey

| Suite | Tests | What it covers |
|---|---|---|
| ValueType | 2 | Enum values, ordering |
| EncodeDecode | 9 | Roundtrip for all field types, edge values, malformed input |
| Comparator | 6 | Each sort field in isolation, equal keys, full sort order |
| Boundary | 4 | 10KB row, empty fields, INT64 MIN/MAX, UserKey null separator |
| Encode | 4 | Byte size correctness, determinism, distinct keys differ |
| EncodeSortOrder | 5 | Bytewise order matches comparator for all four fields |
| DecodeRobustness | 4 | Truncated buffers, garbage stress, negative timestamps |
| ComparatorProperties | 4 | Transitivity, reflexivity, symmetry, row dominance |

### Memtable

| Suite | Tests | What it covers |
|---|---|---|
| Basic | 6 | Put/Get, missing row/col, empty and large values |
| Delete | 5 | Tombstone only, delete after put, put after delete, isolation |
| Versioning | 4 | Newest version returned, all versions in iterator, out-of-order timestamps, upsert |
| MultiColumn | 2 | Independent cols, Bigtable-style column families |
| MultiRow | 3 | Independent rows |
| Iterator | 7 | Empty list, traversal order, timestamp descending, tombstones visible, seek, full count |
| Size | 4 | Non-zero at construction, growth, never shrinks |
| Boundary | 6 | Empty row/col, INT64 MIN/MAX, long keys, negative timestamps |
| Stress | 3 | 1000 rows, 500 versions, iterator count |
| GroundTruth | 2 | 500 random ops verified live against std::map; 1000 ops final state verified |

---

## Roadmap

Implementing the paper bottom-up:

- [x] Arena allocator
- [x] SkipList (MemTable backbone)
- [x] InternalKey — cell identity, sort order, encode/decode for WAL + SSTable
- [x] Memtable — in-memory write buffer with Put/Delete/Get/Iterator interface
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
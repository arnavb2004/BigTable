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
├── utils/
│   └── constants.hpp         ← project-wide tunables (block sizes, thresholds, sentinel values)
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
├── SSTable/
│   ├── sstable.hpp            ← writer, reader, bloom filter, footer, block handle
│   ├── sstable.cpp
│   ├── sstable_test.cpp
│   └── Makefile
└── README.md
```

---

## Components

### 1. Utils

A shared header layer that centralises project-wide tunables and named
constants. No component hardcodes magic values — all tunables are pulled
from here so a single change propagates automatically.

| Constant | Value | Used by |
|---|---|---|
| `kArenaBlockSize` | 4096 | Arena |
| `kSkipListMaxHeight` | 12 | SkipList |
| `kSkipListBranchProb` | 0.25 | SkipList |
| `kMinEncodedKeySize` | 17 | InternalKey::Decode |
| `kMaxTimestamp` | INT64_MAX | Memtable::Get seek sentinel |
| `kMemtableFlushThreshold` | 64 MB | Tablet (upcoming) |
| `kSSTableBlockSize` | 4096 | SSTable |
| `kRestartInterval` | 8 | SSTable |
| `kBloomBitsPerKey` | 10 | SSTable BloomFilter |
| `kBloomNumHashes` | 7 | SSTable BloomFilter |
| `kFooterSize` | 28 | SSTable |
| `kSSTableMagic` | 0xCAFEBABEDEADBEEF | SSTable |
| `kSSTableVersion` | 1 | SSTable |
| `kSSTableTargetFileSize` | 64 MB | Compactor (upcoming) |

All constants live in `namespace bigtable` and are declared `inline constexpr`
to avoid ODR violations when included from multiple translation units.

### 2. Arena allocator

A slab-based memory manager designed for high-frequency, small-object
allocations with minimal overhead — the same strategy used by LevelDB and
RocksDB for their MemTable nodes.

| Property | Detail |
|---|---|
| Block size | `kArenaBlockSize` (4 KB — one OS page) |
| Allocation strategy | Bump-pointer — O(1) per allocation |
| Alignment | 8-byte guaranteed (`AllocateAligned`) |
| Memory tracking | `std::atomic<size_t>` counter — no mutex needed |
| Huge objects | Objects > `kArenaBlockSize / 4` get a dedicated block |

Nodes that don't fit in the current block trigger a fresh 4 KB allocation;
objects larger than 1/4 of the block size get their own dedicated allocation.
The Arena owns all memory and frees it in bulk on destruction — there is no
per-node `delete`.

### 3. SkipList (MemTable backbone)

A probabilistic sorted data structure that serves as the in-memory write
buffer (MemTable) for each tablet. Modelled directly on the LevelDB SkipList.

| Property | Detail |
|---|---|
| Key order | Strictly ascending by comparator |
| Max height | `kSkipListMaxHeight` (12 levels) |
| Promotion probability | `kSkipListBranchProb` (P = 0.25 per level) |
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

### 4. InternalKey (key schema layer)

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

Minimum encoded size is `kMinEncodedKeySize` (17 bytes), used as the first
bounds check in `Decode()`.

### 5. Memtable (in-memory write buffer)

The public write/read interface for a Bigtable tablet. Wraps the SkipList and
Arena behind a clean `Put / Delete / Get` API. Callers never interact with
the SkipList or Arena directly — both are internal implementation details,
swappable without changing the public interface.

| Property | Detail |
|---|---|
| Write path | `Put` / `Delete` → inserts into SkipList |
| Read path | `Get` → seeks to `kMaxTimestamp` version of `(row, col)` |
| Versioning | Every `Put` creates a new version; `Get` always returns newest |
| Tombstones | `Delete` inserts a `kTypeDeletion` marker; pruned at compaction |
| Iterator | Walks all entries in sorted order — used by minor compaction |
| Size tracking | `ApproximateSize()` → Arena usage; Tablet uses this to decide when to flush |
| Flush threshold | `kMemtableFlushThreshold` (64 MB) — checked by Tablet (upcoming) |
| Thread safety | Same contract as SkipList — writes serialised by caller, reads lock-free |

**Ownership model:** Memtable owns its Arena and SkipList. The underlying data
structure is an implementation detail — replacing the SkipList with a B-tree
or adding a Bloom filter requires no changes to the public interface.

### 6. SSTable (immutable on-disk sorted file)

An SSTable is the on-disk representation of a frozen Memtable. Once written
it is never modified — the compactor produces new SSTables by merging existing
ones, never editing them. Implemented as a matched `sstable.hpp` / `sstable.cpp`
pair using the pimpl pattern to keep the public interface stable.

**On-disk layout:**

```
┌──────────────────────────────────────────────────────────┐
│  Data Block 0        (Snappy-compressed¹)                │
│  Filter Block 0      (Bloom filter for Data Block 0)     │
│  Data Block 1        (Snappy-compressed¹)                │
│  Filter Block 1      (Bloom filter for Data Block 1)     │
│  ...                                                     │
│  Index Block         (uncompressed, one entry per block) │
│  Footer              (fixed 28 bytes, always last)       │
└──────────────────────────────────────────────────────────┘
```

¹ Compression is a no-op placeholder with a Snappy-compatible swap-in interface.
Swap two static functions in `sstable.cpp` to enable real compression.

**Data block internal layout (before compression):**
```
Per entry:
  [shared_len   : 4B BE]   bytes shared with previous restart-point key
  [unshared_len : 4B BE]   bytes not shared (suffix appended to prefix)
  [val_len      : 4B BE]
  [key_delta    : unshared_len bytes]
  [value        : val_len bytes]
Trailer:
  [restart_0 : 4B BE] ... [restart_k : 4B BE]
  [num_restarts : 4B BE]
```

**Filter block layout:**
```
[bitset_size : 4B BE][bitset : bitset_size bytes][num_hashes : 1B]
```

**Index block layout:**
```
[num_entries : 4B BE]
Per data block:
  [last_key_size : 4B BE][last_key : last_key_size bytes]
  [block_offset  : 8B BE]
  [block_size    : 4B BE]
  [filter_offset : 8B BE]
  [filter_size   : 4B BE]
```

**Footer layout (always 28 bytes):**
```
[index_offset : 8B BE][index_size : 4B BE][num_blocks : 4B BE]
[magic : 8B BE][version : 4B BE]
```

| Property | Detail |
|---|---|
| Block size | `kSSTableBlockSize` (4 KB — one OS page) |
| Restart interval | 8 entries — one full key every 8 entries within a block |
| Bloom filter key | `UserKey` (`row + '\0' + col`) — one filter entry covers all versions of a cell |
| Index key type | Decoded `InternalKey` (not raw bytes) — enables `InternalKeyComparator` binary search |
| Compression | No-op placeholder; Snappy-compatible interface for swap-in |
| Footer magic | `0xCAFEBABEDEADBEEF` — detects truncated or corrupt files |
| Writer | `SSTableWriter` — streaming; flushes block when uncompressed body exceeds `kSSTableBlockSize` |
| Reader | `SSTableReader` — loads footer + index at open time; data blocks loaded on demand |
| Iterator | Forward-only; decompresses one block at a time — does not hold all blocks in memory |

**Read path for `Get(row, col)`:**
1. Range check — if `row` outside `[smallest_key.row, largest_key.row]` → `kNotFound`
2. `FindBlock()` — binary search index with `InternalKeyComparator`
3. Bloom filter check — load filter block, probe with `UserKey(row, col)`
4. `ReadDataBlock()` — decompress, decode restart-point prefix compression
5. Linear scan from block start for first entry matching `(row, col)`

**Production hardening:**
- `std::streamoff` overflow guard in `ReadAt` — safe on 32-bit platforms
- Footer version field validation — throws `std::runtime_error` with message containing `"version"` on mismatch
- `num_blocks` sanity cap at `kSSTableTargetFileSize / kSSTableBlockSize` (16 384) — prevents runaway `reserve()` on corrupt index
- `shared > current_key.size()` guard in `ReadDataBlock` — throws rather than producing garbage keys
- Per-entry index bounds validation — data block and filter block extents checked against file size at open time
- Overflow-safe arithmetic (`CheckedAdd`, `CheckedMul`, `NarrowToU32`, `ToStreamSize`) throughout write and read paths

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
make sstable

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

# SSTable tests
cd SSTable
g++ -std=c++17 -Wall -Wextra -g sstable_test.cpp sstable.cpp ../MemTable/memtable.cpp ../MemTable/internal_key.cpp ../ArenaAllocator/arena.cpp -o sstable_test
./sstable_test
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

### SSTable

| Suite | Tests | What it covers |
|---|---|---|
| Bloom | 5 | False negatives impossible, false positive rate < 5%, empty/corrupt filter safety, serialised size |
| Footer | 3 | Encode/decode roundtrip, wrong magic rejected, wrong size rejected |
| Writer | 4 | File creation, entry count tracking, file size growth, multi-block output |
| Roundtrip | 7 | Single entry, missing key, tombstone, newest version, tombstone shadows older value, multi-row, Bigtable column families |
| Metadata | 4 | Smallest/largest keys, file size nonzero, block count matches writer, `MayContain` present/absent |
| Iterator | 4 | Forward scan order, Seek, Seek past end, entry count |
| Integration | 2 | Memtable flush → SSTable roundtrip; 500-row flush, all keys readable |
| Boundary | 5 | Empty row/col, INT64_MAX timestamp, empty value, 64KB value, corrupt footer throws |
| GroundTruth | 1 | 1000 random Put/Delete ops flushed to SSTable, every entry verified against std::map |
| IteratorBoundary | 3 | Restart-aligned block boundary, unaligned block boundary, value not corrupted across blocks |
| FileValidation | 8 | Wrong footer version throws with `"version"` in message, correct version opens cleanly, implausible block count throws, plausible count opens cleanly, index offset past footer throws, data block offset outside file throws, corrupt shared prefix throws, truncated/empty file throws |
| SeekPrecision | 5 | Seek before min lands on first entry, seek between blocks lands on correct block, seek to exact first entry of second block, seek past max is invalid, seek then Next walks remaining entries correctly |
| CrossBlockVersions | 4 | Get returns newest when versions span two blocks, three blocks, tombstone in block 0 shadows value in block 1, iterator visits all versions in order |
| DeltaCompression | 4 | Oscillating prefix length, shared prefix collapses to zero mid-group, deeply shared prefix with one-byte divergence, alternating long/short keys with no bleed |

---

## Roadmap

Implementing the paper bottom-up:

- [x] Arena allocator
- [x] SkipList (MemTable backbone)
- [x] InternalKey — cell identity, sort order, encode/decode for WAL + SSTable
- [x] Memtable — in-memory write buffer with Put/Delete/Get/Iterator interface
- [x] Utils — project-wide constants (`kArenaBlockSize`, `kSkipListMaxHeight`, `kMemtableFlushThreshold`, etc.)
- [x] SSTable — immutable on-disk sorted file with block index, Bloom filter, restart-point prefix compression, and production hardening
- [ ] Minor compaction — Memtable → SSTable flush coordinated by Tablet
- [ ] Commit log — append-only WAL, one per tablet server
- [ ] Tablet — owns one Memtable + a list of SSTables; handles the read/write path
- [ ] Major compaction — N SSTables → 1 SSTable merge
- [ ] Tablet server — gRPC server managing N tablets
- [ ] METADATA table — 3-level B+ tree tablet location hierarchy
- [ ] Master server — tablet assignment, load balancing, failure detection

---

## References

- [Bigtable: A Distributed Storage System for Structured Data](https://static.googleusercontent.com/media/research.google.com/en//archive/bigtable-osdi06.pdf) — Chang et al., OSDI 2006
- [LevelDB](https://github.com/google/leveldb) — open-source implementation that shares the same MemTable/SSTable design
#ifndef SSTABLE_HPP
#define SSTABLE_HPP

#include "../MemTable/internal_key.hpp"
#include "../utils/constants.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <cstdint>
#include <memory>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// An SSTable (Sorted String Table) is what a frozen Memtable flushes into.
// Once written it is never modified — the compactor produces new SSTables
// by merging existing ones, it never edits them.
//
// On-disk layout:
//
//   ┌──────────────────────────────────────────────────────────┐
//   │  Data Block 0        (Snappy-compressed)                 │
//   │  Filter Block 0      (Bloom filter for Data Block 0)     │
//   │  Data Block 1        (Snappy-compressed)                 │
//   │  Filter Block 1      (Bloom filter for Data Block 1)     │
//   │  ...                                                     │
//   │  Index Block         (uncompressed, one entry per block) │
//   │  Footer              (fixed 28 bytes, always last)       │
//   └──────────────────────────────────────────────────────────┘
//
// Data block internal layout (before Snappy, after restart encoding):
//   For each entry:
//     [shared_len   : 4B BE]  bytes shared with previous restart-point key
//     [unshared_len : 4B BE]  bytes not shared (suffix appended to prefix)
//     [val_len      : 4B BE]
//     [key_delta    : unshared_len bytes]   ← suffix of InternalKey::Encode()
//     [value        : val_len bytes]
//   Trailer (appended after last entry, before Snappy):
//     [restart_0    : 4B BE] ... [restart_k : 4B BE]  ← offsets within block
//     [num_restarts : 4B BE]
//
// Filter block layout (raw bytes, not compressed):
//   [bitset_size : 4B BE]
//   [bitset      : bitset_size bytes]
//   [num_hashes  : 1B]
//
// Index block layout (uncompressed):
//   [num_entries   : 4B BE]
//   Per data block:
//     [last_key_size  : 4B BE]
//     [last_key       : last_key_size bytes]  ← encoded last InternalKey in block
//     [block_offset   : 8B BE]
//     [block_size     : 4B BE]                ← compressed size on disk
//     [filter_offset  : 8B BE]
//     [filter_size    : 4B BE]
//
// Footer layout (always 28 bytes, always at end of file):
//   [index_offset  : 8B BE]
//   [index_size    : 4B BE]
//   [num_blocks    : 4B BE]
//   [magic         : 8B BE]   ← 0xCAFEBABEDEADBEEF
//   [version       : 4B BE]   ← kSSTableVersion
//
// Key design properties:
//   - Restart interval = 8 (one full key every 8 entries within a block)
//     Optimal for Bigtable-style keys with long shared prefixes.
//   - Bloom filter uses UserKey (row + '\0' + col), not the versioned
//     InternalKey — so a single filter entry covers all versions of a cell.
//   - FindBlock() decodes index last_keys and uses InternalKeyComparator,
//     not raw bytewise comparison — encoded keys do NOT preserve sort order
//     across variable-length row strings.
// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
// 1  Wire-format helpers
//
// Stateless inline functions used by both SSTableWriter and SSTableReader.
// All integers are big-endian on disk.
// ─────────────────────────────────────────────────────────────────────────────

inline void AppendUint32BE(std::string& buf, uint32_t v) {
    buf += static_cast<char>((v >> 24) & 0xFF);
    buf += static_cast<char>((v >> 16) & 0xFF);
    buf += static_cast<char>((v >>  8) & 0xFF);
    buf += static_cast<char>( v        & 0xFF);
}

inline void AppendUint64BE(std::string& buf, uint64_t v) {
    buf += static_cast<char>((v >> 56) & 0xFF);
    buf += static_cast<char>((v >> 48) & 0xFF);
    buf += static_cast<char>((v >> 40) & 0xFF);
    buf += static_cast<char>((v >> 32) & 0xFF);
    buf += static_cast<char>((v >> 24) & 0xFF);
    buf += static_cast<char>((v >> 16) & 0xFF);
    buf += static_cast<char>((v >>  8) & 0xFF);
    buf += static_cast<char>( v        & 0xFF);
}

inline uint32_t DecodeUint32BE(const char* p) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24)
         | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) <<  8)
         |  static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

inline uint64_t DecodeUint64BE(const char* p) {
    return (static_cast<uint64_t>(static_cast<uint8_t>(p[0])) << 56)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[1])) << 48)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[2])) << 40)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[3])) << 32)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[4])) << 24)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[5])) << 16)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[6])) <<  8)
         |  static_cast<uint64_t>(static_cast<uint8_t>(p[7]));
}

// Shared prefix length between two byte strings.
// Used by SSTableWriter to compute key deltas for restart-point compression.
inline size_t SharedPrefixLen(std::string_view a, std::string_view b) {
    size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}


// ─────────────────────────────────────────────────────────────────────────────
// 2  BlockHandle
//
// Records the location of one data block and its paired filter block
// within the file. Stored in the index block so the reader can seek
// directly to any block without scanning.
// ─────────────────────────────────────────────────────────────────────────────

struct BlockHandle {
    InternalKey last_key;     // decoded last key in this data block
                              // used by SSTableReader::FindBlock() for
                              // InternalKeyComparator-based binary search
    uint64_t    offset  = 0;  // byte offset of data block start in file
    uint32_t    size    = 0;  // compressed byte size of data block on disk
    uint64_t    filter_offset = 0;  // byte offset of paired filter block
    uint32_t    filter_size   = 0;  // byte size of paired filter block
};


// ─────────────────────────────────────────────────────────────────────────────
// 3  Footer
//
// Fixed-size trailer always written at the very end of the file.
// SSTableReader seeks to (file_size - kFooterSize) and reads this first,
// then uses index_offset / index_size to load the index block into memory.
// ─────────────────────────────────────────────────────────────────────────────

struct Footer {
    uint64_t index_offset = 0;
    uint32_t index_size   = 0;
    uint32_t num_blocks   = 0;
    uint64_t magic        = bigtable::kSSTableMagic;
    uint32_t version      = bigtable::kSSTableVersion;

    // Serialise to exactly kFooterSize bytes (big-endian).
    std::string Encode() const;

    // Deserialise from exactly kFooterSize bytes.
    // Returns false if magic does not match — indicates a corrupt file.
    static bool Decode(std::string_view buf, Footer& out);
};

inline std::string Footer::Encode() const {
    std::string buf;
    buf.reserve(bigtable::kFooterSize);
    AppendUint64BE(buf, index_offset);
    AppendUint32BE(buf, index_size);
    AppendUint32BE(buf, num_blocks);
    AppendUint64BE(buf, magic);
    AppendUint32BE(buf, version);
    return buf;
}

inline bool Footer::Decode(std::string_view buf, Footer& out) {
    if (buf.size() != bigtable::kFooterSize) return false;
    const char* p  = buf.data();
    out.index_offset = DecodeUint64BE(p); p += 8;
    out.index_size   = DecodeUint32BE(p); p += 4;
    out.num_blocks   = DecodeUint32BE(p); p += 4;
    out.magic        = DecodeUint64BE(p); p += 8;
    out.version      = DecodeUint32BE(p);
    return out.magic == bigtable::kSSTableMagic;
}


// ─────────────────────────────────────────────────────────────────────────────
// 4  BloomFilter  (internal — not part of the public Tablet API)
//
// Probabilistic membership test for user-keys (row + '\0' + col).
// Keyed on user-key, not versioned InternalKey, so one filter entry covers
// all versions and tombstones of a cell.
//
// Parameters (from constants.hpp):
//   kBloomBitsPerKey = 10  →  ~1% false positive rate
//   kBloomNumHashes  = 7   →  optimal for 10 bits/key
//
// Algorithm: Kirsch-Mitzenmacher double hashing.
//   h_i(k) = h1(k) + i * h2(k)  mod m
// Derives k independent hash functions from two base hashes, avoiding
// k separate hash computations. Base hashes use MurmurHash3-style mixing.
//
// Usage (write side — SSTableWriter):
//   BloomFilter bf(num_keys);
//   bf.Add(key.UserKey());       // once per entry
//   std::string raw = bf.Finish();  // serialise → write to disk
//
// Usage (read side — SSTableReader):
//   bool maybe = BloomFilter::MayContain(raw_bytes, user_key);
//   if (!maybe) return kNotFound;  // definite miss — zero disk I/O
// ─────────────────────────────────────────────────────────────────────────────

class BloomFilter {
public:
    // Construct a filter sized for `num_keys` expected insertions.
    explicit BloomFilter(size_t num_keys);

    // Add a user-key to the filter. Call once per entry during block build.
    void Add(std::string_view user_key);

    // Serialise the completed filter to a byte string for writing to disk.
    // Format: [bitset_size : 4B BE][bitset bytes][num_hashes : 1B]
    std::string Finish() const;

    // Query a serialised filter.
    // Returns false → key is definitely absent  (no false negatives guaranteed)
    // Returns true  → key is probably present   (~1% false positive rate)
    static bool MayContain(std::string_view serialised, std::string_view user_key);

private:
    // Two independent 64-bit hashes via MurmurHash3-inspired mixing.
    static std::pair<uint64_t, uint64_t> BaseHashes(std::string_view key);

    std::vector<uint8_t> bits_;
    int                  num_hashes_;
};


// ─────────────────────────────────────────────────────────────────────────────
// 5  SSTableWriter
//
// Writes a sorted sequence of InternalKey → value pairs to an SSTable file.
// Caller (Tablet) is responsible for constructing the file path.
//
// Usage:
//   SSTableWriter writer("/data/tablets/t1/000003.sst");
//   Memtable::Iterator it = mem.NewIterator();
//   for (it.SeekToFirst(); it.Valid(); it.Next())
//       writer.Add(it.key(), it.value());
//   writer.Finish();
//
// Constraints:
//   - Keys must be added in strictly ascending InternalKeyComparator order.
//     The writer does not sort — it trusts the Memtable iterator.
//   - Finish() must be called exactly once after all Add() calls.
//   - Adding keys after Finish() is undefined behaviour.
//   - The writer does NOT call Finish() in the destructor — a half-written
//     SSTable is worse than no SSTable. Caller must call Finish() explicitly.
// ─────────────────────────────────────────────────────────────────────────────

class SSTableWriter {
public:
    // Opens (or creates) the file at `path` for writing.
    // Throws std::runtime_error if the file cannot be opened.
    explicit SSTableWriter(const std::string& path);

    // Does NOT call Finish(). Caller must call Finish() explicitly.
    ~SSTableWriter();

    // Non-copyable — owns a file handle and mutable write state.
    SSTableWriter(const SSTableWriter&)            = delete;
    SSTableWriter& operator=(const SSTableWriter&) = delete;

    // ── Write interface ───────────────────────────────────────────────────────

    // Add one key-value pair.
    // Automatically flushes the current data block (+ its Bloom filter block)
    // when the uncompressed block body exceeds kSSTableBlockSize.
    void Add(const InternalKey& key, const std::string& value);

    // Flush the pending data block, write index block and footer, close file.
    // Must be called exactly once after all Add() calls.
    void Finish();

    // ── Metadata ─────────────────────────────────────────────────────────────

    uint64_t NumEntries() const;   // total entries added via Add()
    uint64_t FileSize()   const;   // bytes written to disk so far
    size_t   NumBlocks()  const;   // data blocks flushed so far

private:
    struct Impl{
        std::ofstream   file_;
        uint64_t        file_offset_  = 0;
        uint64_t        num_entries_  = 0;

        // Current block state
        std::string              block_buf_;    // uncompressed entries so far
        std::string              last_key_;     // encoded last key (for prefix compression)
        int                      entry_count_  = 0;
        std::vector<uint32_t>    restarts_;     // restart point offsets within block_buf_
        std::unique_ptr<BloomFilter> bloom_;    // filter for current block

        std::vector<BlockHandle> index_;        // one entry per flushed block
        bool                     finished_     = false;
    };
    std::unique_ptr<Impl> impl_;

    void FlushBlock();
    void WriteRaw(const std::string& data);
};


// ─────────────────────────────────────────────────────────────────────────────
// 6  SSTableReader
//
// Random-access reader for SSTable files written by SSTableWriter.
//
// On construction (Open):
//   1. Seeks to (file_size - kFooterSize), reads and validates the footer.
//   2. Reads the index block into memory — one BlockHandle per data block.
//      BlockHandles store decoded InternalKeys for comparator-based search.
//   3. Loads smallest_key_ and largest_key_ from index for range checks.
//   Data blocks and filter blocks are loaded on demand.
//
// Read path for Get(row, col):
//   1. Range check — if (row,col) outside [smallest, largest] → kNotFound
//   2. FindBlock() — binary search index with InternalKeyComparator
//   3. Bloom filter check — load filter block, probe with UserKey(row,col)
//   4. ReadDataBlock() — Snappy decompress, binary search restart points
//   5. Linear scan from nearest restart point for exact match
//
// Thread safety: NOT thread-safe. Each thread should use its own SSTableReader.
// ─────────────────────────────────────────────────────────────────────────────

class SSTableReader {
public:
    // Opens the file at `path`, reads footer and index block into memory.
    // Throws std::runtime_error on I/O error or corrupt footer/index.
    explicit SSTableReader(const std::string& path);

    ~SSTableReader();

    // Non-copyable — owns a file handle.
    SSTableReader(const SSTableReader&)            = delete;
    SSTableReader& operator=(const SSTableReader&) = delete;

    // ── Point lookup ──────────────────────────────────────────────────────────

    // Look up the newest entry for (row, col).
    //   kFound    → value_out is populated with the cell value
    //   kDeleted  → newest entry is a tombstone; value_out is unchanged
    //   kNotFound → no entry for (row, col) in this SSTable
    GetResult Get(const std::string& row,
                  const std::string& col,
                  std::string&       value_out);

    // ── Tablet metadata ───────────────────────────────────────────────────────
    // All metadata is loaded at Open() time — no disk I/O after construction.

    // First key in the file (lowest under InternalKeyComparator).
    // Used by Tablet to skip files whose range doesn't overlap the query.
    const InternalKey& SmallestKey() const;

    // Last key in the file (highest under InternalKeyComparator).
    const InternalKey& LargestKey()  const;

    // Probabilistic check: does this SSTable probably contain (row, col)?
    // Returns false → definitely not present, skip this file entirely.
    // Returns true  → probably present, proceed with Get() or Seek().
    // Uses the per-block Bloom filters; no disk I/O.
    bool MayContain(const std::string& row, const std::string& col) const;

    // Total compressed file size in bytes.
    // Used by the compactor to select files for merging.
    uint64_t FileSize()   const;

    // Number of data blocks in this SSTable.
    size_t   NumBlocks()  const;

    // ── Iterator ──────────────────────────────────────────────────────────────
    // Forward-only iterator over every entry in sorted order.
    // Decompresses one data block at a time — does not hold all blocks in memory.
    // Used by the compactor to merge multiple SSTables into one.
    //
    // Usage:
    //   auto it = reader.NewIterator();
    //   for (it.SeekToFirst(); it.Valid(); it.Next()) {
    //       it.key();    // const InternalKey&
    //       it.value();  // const std::string&
    //   }
    class Iterator {
    public:
        explicit Iterator(SSTableReader* reader);

        bool               Valid()  const;
        const InternalKey& key()    const;
        const std::string& value()  const;

        void Next();
        void SeekToFirst();

        // Position at the first entry with key >= target.
        // Used by the compactor to resume mid-file after a split point.
        void Seek(const InternalKey& target);

    private:
        void LoadBlock(size_t block_idx);

        SSTableReader* reader_;
        size_t         block_idx_;
        size_t         entry_idx_;
        std::vector<std::pair<InternalKey, std::string>> entries_;
    };

    Iterator NewIterator();

private:
    struct Impl {
        std::ifstream            file_;
        uint64_t                 file_size_    = 0;
        std::vector<BlockHandle> index_;
        InternalKey              smallest_key_;
        InternalKey              largest_key_;
    };
    std::unique_ptr<Impl> impl_;

    // Read `size` bytes from file at `offset` into out.
    std::string ReadAt(uint64_t offset, uint32_t size);

    // Decompress a data block and decode all its entries.
    std::vector<std::pair<InternalKey, std::string>> ReadDataBlock(size_t block_idx);

    // Binary search index to find the block that may contain (row, col, ts).
    // Returns index_.size() if no block can contain the key.
    size_t FindBlock(const InternalKey& seek_key) const;

    friend class Iterator;
};

#endif
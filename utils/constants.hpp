#ifndef BIGTABLE_CONSTANTS_HPP
#define BIGTABLE_CONSTANTS_HPP

#include <cstdint>
#include <cstddef>
#include <climits>

// ─────────────────────────────────────────────────────────────────────────────
// Project-wide constants for the Bigtable implementation.
//
// All tunables live here so changes propagate automatically.
// Components include this header instead of hardcoding magic values.
// ─────────────────────────────────────────────────────────────────────────────

namespace bigtable {

// ── Arena ─────────────────────────────────────────────────────────────────────

// Default slab size — one OS page (4 KB).
// Objects larger than kArenaBlockSize / 4 get a dedicated allocation.
inline constexpr size_t kArenaBlockSize = 4096;

// ── SkipList ──────────────────────────────────────────────────────────────────

// Maximum number of levels in the skip list.
// Reaching this height is astronomically unlikely at p = 0.25.
// Supports up to ~4^12 ≈ 16M entries before height becomes a bottleneck.
inline constexpr int kSkipListMaxHeight = 12;

// Probability of promoting a node to the next level.
// p = 0.25 → expected height ≈ 1.33, O(log N) search cost.
inline constexpr float kSkipListBranchProb = 0.25f;

// ── InternalKey ───────────────────────────────────────────────────────────────

// Minimum byte length of a valid encoded InternalKey:
//   4 (row_size) + 0 (empty row) + 4 (col_size) + 0 (empty col) + 8 (ts) + 1 (type)
inline constexpr size_t kMinEncodedKeySize = 17;

// Sentinel timestamp used by Memtable::Get to seek to the newest version
// of a (row, col) pair. INT64_MAX is the highest possible timestamp, so
// FindGreaterOrEqual lands at or before the actual newest entry.
inline constexpr int64_t kMaxTimestamp = INT64_MAX;

// ── Memtable ──────────────────────────────────────────────────────────────────

// Freeze and flush the Memtable to SSTable once it reaches this size (bytes).
// 64 MB matches the LevelDB default; tune based on available RAM per tablet.
inline constexpr size_t kMemtableFlushThreshold = 64 * 1024 * 1024;

// ── SSTable ───────────────────────────────────────────────────────────────────

// Target size for each data block before compression.
// 4KB = one OS page; keeps read amplification low.
inline constexpr size_t   kSSTableBlockSize         = 4 * 1024;

// Number of entries between full-key restart points within a block.
// 8 is optimal for Bigtable-style keys with long shared prefixes.
inline constexpr int      kRestartInterval          = 8;

// Bloom filter tuning — 10 bits/key + 7 hash functions → ~1% false positive rate.
inline constexpr int      kBloomBitsPerKey          = 10;
inline constexpr int      kBloomNumHashes           = 7;

// Footer is always 28 bytes at the end of every SSTable file.
//   index_offset : 8B
//   index_size   : 4B
//   num_blocks   : 4B
//   magic        : 8B
//   version      : 4B
inline constexpr size_t   kFooterSize               = 28;

// Sanity marker written into every footer. Detects truncated/corrupt files.
inline constexpr uint64_t kSSTableMagic             = 0xCAFEBABEDEADBEEFULL;

// Current on-disk format version. Bump when the layout changes.
inline constexpr uint32_t kSSTableVersion           = 1;

// Target SSTable file size — matches kMemtableFlushThreshold so one Memtable
// flush produces one SSTable. Compactor uses this for output file boundaries.
inline constexpr size_t   kSSTableTargetFileSize    = 64 * 1024 * 1024;  // 64 MB

}

#endif
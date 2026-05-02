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

}

#endif
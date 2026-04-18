#ifndef INTERNAL_KEY_HPP
#define INTERNAL_KEY_HPP

#include <string>
#include <string_view>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Defines the Bigtable key schema — the WHAT layer.
// Knows nothing about SkipList, Arena, or Memtable.
//
// Every cell in Bigtable is identified by:
//   (row, column_family:qualifier, timestamp, type)
//
// This file owns:
//   - ValueType   : whether an entry is a value or a deletion tombstone
//   - InternalKey : the struct holding all four fields
//   - Encode()    : packs InternalKey → binary string (sort-preserving)
//   - Decode()    : unpacks binary string → InternalKey
//   - InternalKeyComparator : comparator for the SkipList
// ─────────────────────────────────────────────────────────────────────────────


// ── ValueType ─────────────────────────────────────────────────────────────────
// Tags every SkipList entry as either a real value or a deletion tombstone.
// kTypeDeletion = 0, kTypeValue = 1  (Value > Deletion so Value sorts first
// at the same timestamp — newest live value beats a tombstone at same ts).
enum ValueType : uint8_t {
    kTypeDeletion = 0,
    kTypeValue    = 1
};


// ── InternalKey ───────────────────────────────────────────────────────────────
// The logical representation of a Bigtable key in memory.
// Never stored directly in the SkipList — always Encode()'d first.
struct InternalKey {
    std::string row;        // e.g. "com.google.www"
    std::string col;        // e.g. "contents:html"  (family:qualifier)
    int64_t     timestamp;  // unix micros; higher = newer
    ValueType   type;       // kTypeValue or kTypeDeletion

    // Default constructor — needed for SkipList sentinel head node
    InternalKey() : timestamp(0), type(kTypeValue) {}

    InternalKey(std::string row, std::string col,
                int64_t ts, ValueType t)
        : row(std::move(row)), col(std::move(col)),
          timestamp(ts), type(t) {}
    
    // Returns row + '\0' + col.
    // Null byte is an unambiguous separator — user strings won't contain it.
    // Used by Memtable::Get to detect when iterator moves past target row/col.
    std::string UserKey() const;

    bool operator==(const InternalKey& o) const {
        return row == o.row && col == o.col &&
               timestamp == o.timestamp && type == o.type;
    }

    // Encode this key into a single binary string suitable for SkipList storage.
    // The encoding preserves sort order: row ASC, col ASC, ts DESC, type DESC.
    // Format: [row_size:4B][row][col_size:4B][col][~timestamp:8B][~type:1B]
    // Timestamp and type are bitwise-complemented so that higher values
    // sort first in a bytewise comparison.
    std::string Encode() const;

    // Decode a binary string produced by Encode() back into an InternalKey.
    // Returns false if the buffer is malformed.
    static bool Decode(const std::string_view encoded, InternalKey& out);
};


// ── InternalKeyComparator ─────────────────────────────────────────────────────
// Comparator passed to SkipList<InternalKey, ...>.
// Sort order:
//   1. row        — ascending  (lexicographic)
//   2. col        — ascending  (lexicographic)
//   3. timestamp  — descending (newest first)
//   4. type       — descending (kTypeValue before kTypeDeletion at same ts)
struct InternalKeyComparator {
    int operator()(const InternalKey& a, const InternalKey& b) const;
};


#endif // INTERNAL_KEY_HPP
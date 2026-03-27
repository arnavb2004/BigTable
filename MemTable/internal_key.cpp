#include "internal_key.hpp"
#include <cstring>  // memcpy
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// internal_key.cpp
//
// Implements InternalKey::UserKey(), InternalKey::Encode(), InternalKey::Decode(),
// and InternalKeyComparator::operator().
// ─────────────────────────────────────────────────────────────────────────────


//──UserKey ───────────────────────────────────────────────────────────────────-
// Returns row + '\0' + col.
// The null byte separator is unambiguous because user-facing row and col
// strings will not contain null bytes.
//
// Example:
//   row="com.google.www", col="contents:html"
//   UserKey() = "com.google.www\0contents:html"
//
// Used by Memtable::Get to check if the iterator has moved past the
// target (row, col) during a point lookup scan.
std::string InternalKey::UserKey() const {
    return row + '\0' + col;
}


// ── Encode ────────────────────────────────────────────────────────────────────
// Packs (row, col, timestamp, type) into a single binary string.
//
// Binary layout:
//   [row_size : 4 bytes, big-endian]
//   [row      : row_size bytes     ]
//   [col_size : 4 bytes, big-endian]
//   [col      : col_size bytes     ]
//   [~ts      : 8 bytes, big-endian]   ← bitwise complement for DESC sort
//   [~type    : 1 byte             ]   ← bitwise complement for DESC sort
//
// Why big-endian for sizes and the complemented fields?
// The SkipList comparator works on InternalKey structs directly, so the binary
// encoding is used for WAL and SSTable serialisation — not for in-memory
// comparison. However the encoding must still be unambiguous and reversible.
std::string InternalKey::Encode() const {
    // TODO: implement
    // 1. Reserve total size upfront to avoid reallocation
    // 2. Write row_size as 4-byte big-endian
    // 3. Write row bytes
    // 4. Write col_size as 4-byte big-endian
    // 5. Write col bytes
    // 6. Write ~timestamp as 8-byte big-endian
    // 7. Write ~type as 1 byte
    return "";
}


// ── Decode ────────────────────────────────────────────────────────────────────
// Reverses Encode(). Returns false if buffer is too short or malformed.
bool InternalKey::Decode(const std::string_view encoded, InternalKey& out) {
    // TODO: implement
    // 1. Check minimum size (4 + 0 + 4 + 0 + 8 + 1 = 17 bytes)
    // 2. Read row_size, extract row
    // 3. Read col_size, extract col
    // 4. Read ~timestamp, complement back
    // 5. Read ~type, complement back, cast to ValueType
    // 6. Return false at any bounds violation
    return false;
}


// ── InternalKeyComparator ─────────────────────────────────────────────────────
// Returns negative if a < b, zero if a == b, positive if a > b.
int InternalKeyComparator::operator()(const InternalKey& a,
                                      const InternalKey& b) const {
    // TODO: implement
    // 1. Compare row  (ascending)  — return if not equal
    // 2. Compare col  (ascending)  — return if not equal
    // 3. Compare ts   (descending) — higher ts comes first
    // 4. Compare type (descending) — kTypeValue before kTypeDeletion
    return 0;
}
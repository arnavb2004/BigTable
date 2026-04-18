#include "internal_key.hpp"
#include <cstring>  // memcpy
#include <string_view>

// Implements InternalKey::UserKey(), InternalKey::Encode(), InternalKey::Decode(),
// and InternalKeyComparator::operator().
// ─────────────────────────────────────────────────────────────────────────────


//── UserKey ───────────────────────────────────────────────────────────────────-
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
    const uint32_t row_size = static_cast<uint32_t>(row.size());
    const uint32_t col_size = static_cast<uint32_t>(col.size());
 
    // Total size: 4 + row + 4 + col + 8 + 1
    const size_t total = 4 + row_size + 4 + col_size + 8 + 1;
    std::string buf;
    buf.resize(total);
 
    size_t pos = 0;
 
    // Helper lambda: write a value of type T as big-endian bytes
    auto write_be = [&]<typename T>(T val) {
        for (int i = sizeof(T) - 1; i >= 0; --i) {
            buf[pos++] = static_cast<char>((val >> (8 * i)) & 0xFF);
        }
    };
 
    // 1. row_size (4 bytes BE)
    write_be(row_size);
 
    // 2. row bytes
    std::memcpy(&buf[pos], row.data(), row_size);
    pos += row_size;
 
    // 3. col_size (4 bytes BE)
    write_be(col_size);
 
    // 4. col bytes
    std::memcpy(&buf[pos], col.data(), col_size);
    pos += col_size;
 
    // 5. ~timestamp (8 bytes BE) — complement so higher ts sorts first
    write_be(~static_cast<uint64_t>(timestamp));
 
    // 6. ~type (1 byte) — complement so kTypeValue(1) sorts before kTypeDeletion(0)
    buf[pos++] = static_cast<char>(~static_cast<uint8_t>(type));
 
    return buf;
}


// ── Decode ────────────────────────────────────────────────────────────────────
// Reverses Encode(). Returns false if buffer is too short or malformed.
bool InternalKey::Decode(const std::string_view encoded, InternalKey& out) {
    const size_t len = encoded.size();
    size_t pos = 0;
 
    // Minimum size: 4 (row_size) + 0 (row) + 4 (col_size) + 0 (col) + 8 (ts) + 1 (type)
    if (len < 17) return false;
 
    // Helper lambda: read big-endian value of type T
    auto read_be = [&]<typename T>() -> T {
        T val = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            val = (val << 8) | static_cast<uint8_t>(encoded[pos++]);
        }
        return val;
    };
 
    // 1. row_size
    uint32_t row_size = read_be.operator()<uint32_t>();
    if (pos + row_size > len) return false;
 
    // 2. row
    out.row = std::string(encoded.data() + pos, row_size);
    pos += row_size;
 
    // Bounds check before reading col_size
    if (pos + 4 > len) return false;
 
    // 3. col_size
    uint32_t col_size = read_be.operator()<uint32_t>();
    if (pos + col_size > len) return false;
 
    // 4. col
    out.col = std::string(encoded.data() + pos, col_size);
    pos += col_size;
 
    // Bounds check before reading timestamp + type
    if (pos + 9 > len) return false;
 
    // 5. ~timestamp → complement back to original
    uint64_t ts_complemented = read_be.operator()<uint64_t>();
    out.timestamp = static_cast<int64_t>(~ts_complemented);
 
    // 6. ~type → complement back, cast to ValueType
    uint8_t type_complemented = static_cast<uint8_t>(encoded[pos++]);
    out.type = static_cast<ValueType>(~type_complemented & 0xFF);
 
    return true;
}


// ── InternalKeyComparator ─────────────────────────────────────────────────────
// Returns negative if a < b, zero if a == b, positive if a > b.
int InternalKeyComparator::operator()(const InternalKey& a,
                                      const InternalKey& b) const {
    // 1. row ascending
    if (int cmp = a.row.compare(b.row); cmp != 0) return cmp;
 
    // 2. col ascending
    if (int cmp = a.col.compare(b.col); cmp != 0) return cmp;
 
    // 3. timestamp descending — higher timestamp sorts first
    if (a.timestamp != b.timestamp)
        return (a.timestamp > b.timestamp) ? -1 : 1;
 
    // 4. type descending — kTypeValue(1) before kTypeDeletion(0)
    if (a.type != b.type)
        return (a.type > b.type) ? -1 : 1;
 
    return 0;
}
#include "internal_key.hpp"
#include <algorithm>
#include <iostream>
#include <cassert>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Test framework
// ─────────────────────────────────────────────────────────────────────────────
static int g_total  = 0;
static int g_passed = 0;
static int g_failed = 0;

#define EXPECT_TRUE(expr) do { \
    ++g_total; \
    if (!(expr)) { \
        ++g_failed; \
        std::cerr << "  FAIL [" << __LINE__ << "]: " << #expr << "\n"; \
    } else { ++g_passed; } \
} while(0)

#define EXPECT_EQ(a,b)   EXPECT_TRUE((a) == (b))
#define EXPECT_NE(a,b)   EXPECT_TRUE((a) != (b))
#define EXPECT_LT(a,b)   EXPECT_TRUE((a) <  (b))
#define EXPECT_GT(a,b)   EXPECT_TRUE((a) >  (b))
#define EXPECT_FALSE(e)  EXPECT_TRUE(!(e))

#define TEST(suite, name) \
    static void suite##_##name(); \
    struct suite##_##name##_reg { \
        suite##_##name##_reg() { \
            std::cout << "[" #suite "] " #name "\n"; \
            suite##_##name(); \
        } \
    } suite##_##name##_instance; \
    static void suite##_##name()

// ─────────────────────────────────────────────────────────────────────────────
// Suite 1: ValueType
// ─────────────────────────────────────────────────────────────────────────────
 
TEST(ValueType, enum_values) {
    EXPECT_EQ(static_cast<int>(kTypeDeletion), 0);
    EXPECT_EQ(static_cast<int>(kTypeValue),    1);
}
 
TEST(ValueType, value_greater_than_deletion) {
    // kTypeValue sorts before kTypeDeletion in descending order,
    // which means kTypeValue > kTypeDeletion numerically.
    EXPECT_GT(kTypeValue, kTypeDeletion);
}
 
// ─────────────────────────────────────────────────────────────────────────────
// Suite 2: Encode / Decode roundtrip
// ─────────────────────────────────────────────────────────────────────────────
 
TEST(EncodeDecode, normal_key_roundtrip) {
    InternalKey k{"com.google.www", "contents:html", 1000, kTypeValue};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.row,       k.row);
    EXPECT_EQ(out.col,       k.col);
    EXPECT_EQ(out.timestamp, k.timestamp);
    EXPECT_EQ(out.type,      k.type);
}
 
TEST(EncodeDecode, empty_row) {
    InternalKey k{"", "col", 42, kTypeDeletion};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.row,       k.row);
    EXPECT_EQ(out.col,       k.col);
    EXPECT_EQ(out.timestamp, k.timestamp);
    EXPECT_EQ(out.type,      k.type);
}
 
TEST(EncodeDecode, empty_col) {
    InternalKey k{"row", "", 99, kTypeValue};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.row,       k.row);
    EXPECT_EQ(out.col,       k.col);
    EXPECT_EQ(out.timestamp, k.timestamp);
    EXPECT_EQ(out.type,      k.type);
}
 
TEST(EncodeDecode, timestamp_zero) {
    InternalKey k{"r", "c", 0, kTypeValue};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.timestamp, 0);
}
 
TEST(EncodeDecode, timestamp_int64_max) {
    InternalKey k{"r", "c", INT64_MAX, kTypeValue};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.timestamp, INT64_MAX);
}
 
TEST(EncodeDecode, timestamp_int64_min) {
    InternalKey k{"r", "c", INT64_MIN, kTypeDeletion};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.timestamp, INT64_MIN);
    EXPECT_EQ(out.type,      kTypeDeletion);
}
 
TEST(EncodeDecode, decode_malformed_buffer) {
    // Random garbage — too short to be valid
    std::string garbage = "xyz";
    InternalKey out;
    EXPECT_FALSE(InternalKey::Decode(garbage, out));
}
 
TEST(EncodeDecode, decode_empty_buffer) {
    InternalKey out;
    EXPECT_FALSE(InternalKey::Decode("", out));
}
 
TEST(EncodeDecode, deletion_type_roundtrip) {
    InternalKey k{"row", "col", 500, kTypeDeletion};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.type, kTypeDeletion);
}
 
// ─────────────────────────────────────────────────────────────────────────────
// Suite 3: Comparator — field-by-field ordering
// ─────────────────────────────────────────────────────────────────────────────
 
TEST(Comparator, row_ascending) {
    InternalKeyComparator cmp;
    InternalKey a{"a", "col", 100, kTypeValue};
    InternalKey b{"b", "col", 100, kTypeValue};
    EXPECT_LT(cmp(a, b), 0);
    EXPECT_GT(cmp(b, a), 0);
}
 
TEST(Comparator, col_ascending_same_row) {
    InternalKeyComparator cmp;
    InternalKey a{"row", "aaa", 100, kTypeValue};
    InternalKey b{"row", "bbb", 100, kTypeValue};
    EXPECT_LT(cmp(a, b), 0);
    EXPECT_GT(cmp(b, a), 0);
}
 
TEST(Comparator, timestamp_descending) {
    // Higher timestamp must sort first (return negative when a.ts > b.ts)
    InternalKeyComparator cmp;
    InternalKey a{"row", "col", 200, kTypeValue};
    InternalKey b{"row", "col", 100, kTypeValue};
    EXPECT_LT(cmp(a, b), 0);  // newer a comes before older b
    EXPECT_GT(cmp(b, a), 0);
}
 
TEST(Comparator, type_descending_same_ts) {
    // kTypeValue(1) must sort before kTypeDeletion(0) at same timestamp
    InternalKeyComparator cmp;
    InternalKey a{"row", "col", 100, kTypeValue};
    InternalKey b{"row", "col", 100, kTypeDeletion};
    EXPECT_LT(cmp(a, b), 0);
    EXPECT_GT(cmp(b, a), 0);
}
 
TEST(Comparator, equal_keys) {
    InternalKeyComparator cmp;
    InternalKey a{"row", "col", 100, kTypeValue};
    InternalKey b{"row", "col", 100, kTypeValue};
    EXPECT_EQ(cmp(a, b), 0);
}
 
// ─────────────────────────────────────────────────────────────────────────────
// Suite 4: Comparator — end-to-end sort matches Bigtable paper
// ─────────────────────────────────────────────────────────────────────────────
 
TEST(Comparator, full_sort_order) {
    InternalKeyComparator cmp;
 
    // Build an intentionally scrambled list
    std::vector<InternalKey> keys = {
        {"com.google.www", "contents:html", 100, kTypeDeletion},
        {"com.google.www", "contents:html", 200, kTypeValue},    // newest → first
        {"com.apple.www",  "contents:html", 100, kTypeValue},    // smaller row → before google
        {"com.google.www", "anchor:cnnsi",  100, kTypeValue},    // smaller col → before contents
        {"com.google.www", "contents:html", 100, kTypeValue},    // same ts, Value before Deletion
    };
 
    std::sort(keys.begin(), keys.end(), [&](const InternalKey& a, const InternalKey& b) {
        return cmp(a, b) < 0;
    });
 
    // Expected order:
    // 0: com.apple.www  / contents:html / 100 / Value
    // 1: com.google.www / anchor:cnnsi  / 100 / Value
    // 2: com.google.www / contents:html / 200 / Value   ← highest ts first
    // 3: com.google.www / contents:html / 100 / Value   ← Value before Deletion
    // 4: com.google.www / contents:html / 100 / Deletion
    EXPECT_EQ(keys[0].row, "com.apple.www");
    EXPECT_EQ(keys[1].col, "anchor:cnnsi");
    EXPECT_EQ(keys[2].timestamp, 200);
    EXPECT_EQ(keys[3].type, kTypeValue);
    EXPECT_EQ(keys[4].type, kTypeDeletion);
}
 
// ─────────────────────────────────────────────────────────────────────────────
// Suite 5: Boundary
// ─────────────────────────────────────────────────────────────────────────────
 
TEST(Boundary, very_long_row_key) {
    std::string long_row(10 * 1024, 'x');  // 10 KB
    InternalKey k{long_row, "col", 1, kTypeValue};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.row, long_row);
}
 
TEST(Boundary, both_empty_row_and_col) {
    InternalKey k{"", "", 0, kTypeValue};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.row, "");
    EXPECT_EQ(out.col, "");
    EXPECT_EQ(out.timestamp, 0);
}
 
TEST(Boundary, int64_min_and_max_sort_order) {
    // INT64_MAX timestamp must sort before INT64_MIN
    InternalKeyComparator cmp;
    InternalKey newest{"row", "col", INT64_MAX, kTypeValue};
    InternalKey oldest{"row", "col", INT64_MIN, kTypeValue};
    EXPECT_LT(cmp(newest, oldest), 0);
    EXPECT_GT(cmp(oldest, newest), 0);
}
 
TEST(Boundary, userkey_contains_null_separator) {
    InternalKey k{"abc", "def", 0, kTypeValue};
    std::string uk = k.UserKey();
    // Must be "abc\0def" — length 7, null at index 3
    EXPECT_EQ(uk.size(), 7u);
    EXPECT_EQ(uk[3], '\0');
    EXPECT_EQ(uk.substr(0, 3), "abc");
    EXPECT_EQ(uk.substr(4),    "def");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 6: Encode correctness
// ─────────────────────────────────────────────────────────────────────────────

TEST(Encode, output_size_is_exact) {
    // Total = 4 + row.size() + 4 + col.size() + 8 + 1
    InternalKey k{"hello", "world", 100, kTypeValue};
    EXPECT_EQ(k.Encode().size(), 4 + 5 + 4 + 5 + 8 + 1);
}

TEST(Encode, empty_fields_size_is_exact) {
    InternalKey k{"", "", 0, kTypeValue};
    EXPECT_EQ(k.Encode().size(), 4 + 0 + 4 + 0 + 8 + 1);  // 17 bytes minimum
}

TEST(Encode, deterministic_same_key_same_bytes) {
    InternalKey k{"com.google.www", "contents:html", 12345, kTypeValue};
    EXPECT_EQ(k.Encode(), k.Encode());
}

TEST(Encode, different_keys_different_bytes) {
    InternalKey a{"row1", "col", 100, kTypeValue};
    InternalKey b{"row2", "col", 100, kTypeValue};
    EXPECT_NE(a.Encode(), b.Encode());
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 7: Encode sort order matches comparator
//
// This is the most important correctness property of Encode():
// bytewise comparison of encoded strings must agree with InternalKeyComparator.
// If this breaks, SSTable binary search will return wrong results.
// ─────────────────────────────────────────────────────────────────────────────

TEST(EncodeSortOrder, row_order_preserved) {
    InternalKey a{"apple", "col", 100, kTypeValue};
    InternalKey b{"banana", "col", 100, kTypeValue};
    // comparator says a < b, encoded bytes must also say a < b
    EXPECT_LT(a.Encode(), b.Encode());
}

TEST(EncodeSortOrder, col_order_preserved) {
    InternalKey a{"row", "aaa", 100, kTypeValue};
    InternalKey b{"row", "zzz", 100, kTypeValue};
    EXPECT_LT(a.Encode(), b.Encode());
}

TEST(EncodeSortOrder, timestamp_descending_preserved) {
    // Higher timestamp → smaller encoded bytes (complement trick)
    InternalKey newer{"row", "col", 200, kTypeValue};
    InternalKey older{"row", "col", 100, kTypeValue};
    EXPECT_LT(newer.Encode(), older.Encode());
}

TEST(EncodeSortOrder, type_descending_preserved) {
    // kTypeValue → smaller encoded bytes than kTypeDeletion
    InternalKey val{"row", "col", 100, kTypeValue};
    InternalKey del{"row", "col", 100, kTypeDeletion};
    EXPECT_LT(val.Encode(), del.Encode());
}

TEST(EncodeSortOrder, full_order_matches_comparator) {
    InternalKeyComparator cmp;
    std::vector<InternalKey> keys = {
        {"com.google.www", "contents:html", 100, kTypeDeletion},
        {"com.google.www", "contents:html", 200, kTypeValue},
        {"com.apple.www",  "contents:html", 100, kTypeValue},
        {"com.google.www", "anchor:cnnsi",  100, kTypeValue},
        {"com.google.www", "contents:html", 100, kTypeValue},
    };

    // Sort by comparator
    std::vector<InternalKey> by_cmp = keys;
    std::sort(by_cmp.begin(), by_cmp.end(), [&](const InternalKey& a, const InternalKey& b) {
        return cmp(a, b) < 0;
    });

    // Sort by encoded bytes
    std::vector<InternalKey> by_enc = keys;
    std::sort(by_enc.begin(), by_enc.end(), [](const InternalKey& a, const InternalKey& b) {
        return a.Encode() < b.Encode();
    });

    // Both sorts must produce identical ordering
    for (size_t i = 0; i < by_cmp.size(); ++i) {
        EXPECT_EQ(by_cmp[i].row,       by_enc[i].row);
        EXPECT_EQ(by_cmp[i].col,       by_enc[i].col);
        EXPECT_EQ(by_cmp[i].timestamp, by_enc[i].timestamp);
        EXPECT_EQ(by_cmp[i].type,      by_enc[i].type);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 8: Decode robustness
// ─────────────────────────────────────────────────────────────────────────────

TEST(DecodeRobustness, truncated_after_row_size) {
    // Buffer is 17+ bytes but row_size claims more than available
    InternalKey k{"averylongrowkey", "col", 100, kTypeValue};
    std::string encoded = k.Encode();
    // Chop off enough bytes to make row data unavailable
    encoded.resize(encoded.size() - 10);
    InternalKey out;
    EXPECT_FALSE(InternalKey::Decode(encoded, out));
}

TEST(DecodeRobustness, exactly_17_bytes_empty_strings) {
    // Minimum valid buffer: empty row, empty col
    InternalKey k{"", "", 0, kTypeValue};
    std::string encoded = k.Encode();
    EXPECT_EQ(encoded.size(), 17u);
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(encoded, out));
}

TEST(DecodeRobustness, garbage_stress_never_crashes) {
    InternalKey out;
    // All lengths from 0..31, filled with 0xFF — must never crash
    for (size_t len = 0; len < 32; ++len) {
        std::string garbage(len, '\xFF');
        InternalKey::Decode(garbage, out);  // return value irrelevant — must not crash
    }
    EXPECT_TRUE(true);  // reaching here means no crash
}

TEST(DecodeRobustness, negative_timestamp_roundtrip) {
    InternalKey k{"row", "col", -42, kTypeValue};
    InternalKey out;
    EXPECT_TRUE(InternalKey::Decode(k.Encode(), out));
    EXPECT_EQ(out.timestamp, -42);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 9: Comparator properties
// ─────────────────────────────────────────────────────────────────────────────

TEST(ComparatorProperties, transitivity) {
    InternalKeyComparator cmp;
    InternalKey a{"apple",  "col", 300, kTypeValue};
    InternalKey b{"banana", "col", 200, kTypeValue};
    InternalKey c{"cherry", "col", 100, kTypeValue};
    // a < b and b < c must imply a < c
    EXPECT_LT(cmp(a, b), 0);
    EXPECT_LT(cmp(b, c), 0);
    EXPECT_LT(cmp(a, c), 0);
}

TEST(ComparatorProperties, reflexivity) {
    InternalKeyComparator cmp;
    InternalKey a{"row", "col", 100, kTypeValue};
    EXPECT_EQ(cmp(a, a), 0);
}

TEST(ComparatorProperties, symmetry) {
    InternalKeyComparator cmp;
    InternalKey a{"row1", "col", 100, kTypeValue};
    InternalKey b{"row2", "col", 100, kTypeValue};
    // if cmp(a,b) < 0 then cmp(b,a) > 0
    EXPECT_LT(cmp(a, b), 0);
    EXPECT_GT(cmp(b, a), 0);
}

TEST(ComparatorProperties, only_row_differs_col_ignored) {
    // Two keys with same col/ts/type but different rows — row must dominate
    InternalKeyComparator cmp;
    InternalKey a{"aaa", "zzz", 999, kTypeValue};
    InternalKey b{"bbb", "aaa",   1, kTypeDeletion};
    // a.row < b.row so a must sort first regardless of other fields
    EXPECT_LT(cmp(a, b), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "\n=== InternalKey Tests ===\n\n";
    if (g_failed == 0) {
        std::cout << "Results : " << g_passed << " / " << g_total << " assertions passed\n";
        std::cout << "ALL TESTS PASSED \n\n";
        return 0;
    }
    std::cout << "Results : " << g_passed << " / " << g_total << " assertions passed\n";
    std::cout << g_failed << " ASSERTION(S) FAILED \n\n";
    return 1;
}
#include "memtable.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <climits>

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
        std::cerr << "  FAIL [line " << __LINE__ << "]: " << #expr << "\n"; \
    } else { ++g_passed; } \
} while(0)

#define EXPECT_EQ(a,b)  EXPECT_TRUE((a) == (b))
#define EXPECT_NE(a,b)  EXPECT_TRUE((a) != (b))
#define EXPECT_LT(a,b)  EXPECT_TRUE((a) <  (b))
#define EXPECT_GT(a,b)  EXPECT_TRUE((a) >  (b))
#define EXPECT_FALSE(e) EXPECT_TRUE(!(e))

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
// Suite 1: Basic Put and Get
// ─────────────────────────────────────────────────────────────────────────────

TEST(Basic, put_then_get_returns_found) {
    Memtable mem;
    mem.Put("row", "col", 100, "value");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "value");
}

TEST(Basic, missing_key_returns_not_found) {
    Memtable mem;
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kNotFound);
}

TEST(Basic, missing_col_returns_not_found) {
    Memtable mem;
    mem.Put("row", "col1", 100, "value");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col2", out), GetResult::kNotFound);
}

TEST(Basic, missing_row_returns_not_found) {
    Memtable mem;
    mem.Put("row1", "col", 100, "value");
    std::string out;
    EXPECT_EQ(mem.Get("row2", "col", out), GetResult::kNotFound);
}

TEST(Basic, empty_value_stored_correctly) {
    Memtable mem;
    mem.Put("row", "col", 100, "");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "");
}

TEST(Basic, large_value_stored_correctly) {
    Memtable mem;
    std::string big(1024 * 64, 'x');  // 64KB value
    mem.Put("row", "col", 100, big);
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, big);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 2: Delete / tombstone
// ─────────────────────────────────────────────────────────────────────────────

TEST(Delete, delete_only_returns_deleted) {
    Memtable mem;
    mem.Delete("row", "col", 100);
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kDeleted);
}

TEST(Delete, delete_after_put_returns_deleted) {
    Memtable mem;
    mem.Put("row", "col", 100, "value");
    mem.Delete("row", "col", 200);  // newer timestamp shadows the Put
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kDeleted);
}

TEST(Delete, put_after_delete_returns_found) {
    Memtable mem;
    mem.Delete("row", "col", 100);
    mem.Put("row", "col", 200, "revived");  // newer than tombstone
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "revived");
}

TEST(Delete, delete_does_not_affect_other_cols) {
    Memtable mem;
    mem.Put("row", "col1", 100, "value1");
    mem.Put("row", "col2", 100, "value2");
    mem.Delete("row", "col1", 200);
    std::string out;
    EXPECT_EQ(mem.Get("row", "col1", out), GetResult::kDeleted);
    EXPECT_EQ(mem.Get("row", "col2", out), GetResult::kFound);
    EXPECT_EQ(out, "value2");
}

TEST(Delete, delete_does_not_affect_other_rows) {
    Memtable mem;
    mem.Put("row1", "col", 100, "value1");
    mem.Put("row2", "col", 100, "value2");
    mem.Delete("row1", "col", 200);
    std::string out;
    EXPECT_EQ(mem.Get("row1", "col", out), GetResult::kDeleted);
    EXPECT_EQ(mem.Get("row2", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "value2");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 3: Versioning — multiple versions, Get returns newest
// ─────────────────────────────────────────────────────────────────────────────

TEST(Versioning, get_returns_newest_version) {
    Memtable mem;
    mem.Put("row", "col", 100, "v1");
    mem.Put("row", "col", 200, "v2");
    mem.Put("row", "col", 300, "v3");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "v3");
}

TEST(Versioning, older_versions_still_exist_in_iterator) {
    Memtable mem;
    mem.Put("row", "col", 100, "v1");
    mem.Put("row", "col", 200, "v2");
    mem.Put("row", "col", 300, "v3");

    // Count all versions via iterator
    auto it = mem.NewIterator();
    int count = 0;
    for (it.SeekToFirst(); it.Valid(); it.Next()) {
        ++count;
    }
    EXPECT_EQ(count, 3);
}

TEST(Versioning, insert_out_of_order_timestamps) {
    Memtable mem;
    mem.Put("row", "col", 300, "v3");
    mem.Put("row", "col", 100, "v1");
    mem.Put("row", "col", 200, "v2");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "v3");  // still returns highest ts
}

TEST(Versioning, same_ts_upserts_value) {
    Memtable mem;
    mem.Put("row", "col", 100, "original");
    mem.Put("row", "col", 100, "updated");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "updated");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 4: Multiple columns and rows
// ─────────────────────────────────────────────────────────────────────────────

TEST(MultiColumn, same_row_different_cols_independent) {
    Memtable mem;
    mem.Put("row", "col1", 100, "v1");
    mem.Put("row", "col2", 100, "v2");
    mem.Put("row", "col3", 100, "v3");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col1", out), GetResult::kFound); EXPECT_EQ(out, "v1");
    EXPECT_EQ(mem.Get("row", "col2", out), GetResult::kFound); EXPECT_EQ(out, "v2");
    EXPECT_EQ(mem.Get("row", "col3", out), GetResult::kFound); EXPECT_EQ(out, "v3");
}

TEST(MultiColumn, bigtable_style_column_families) {
    Memtable mem;
    mem.Put("com.google.www", "contents:html",  1000, "<html>");
    mem.Put("com.google.www", "contents:size",  1000, "1024");
    mem.Put("com.google.www", "anchor:cnnsi",   1000, "CNN");
    mem.Put("com.google.www", "anchor:my.look", 1000, "look");
    std::string out;
    EXPECT_EQ(mem.Get("com.google.www", "contents:html",  out), GetResult::kFound);
    EXPECT_EQ(out, "<html>");
    EXPECT_EQ(mem.Get("com.google.www", "anchor:cnnsi", out), GetResult::kFound);
    EXPECT_EQ(out, "CNN");
}

TEST(MultiRow, different_rows_independent) {
    Memtable mem;
    mem.Put("row1", "col", 100, "v1");
    mem.Put("row2", "col", 100, "v2");
    mem.Put("row3", "col", 100, "v3");
    std::string out;
    EXPECT_EQ(mem.Get("row1", "col", out), GetResult::kFound); EXPECT_EQ(out, "v1");
    EXPECT_EQ(mem.Get("row2", "col", out), GetResult::kFound); EXPECT_EQ(out, "v2");
    EXPECT_EQ(mem.Get("row3", "col", out), GetResult::kFound); EXPECT_EQ(out, "v3");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 5: Iterator
// ─────────────────────────────────────────────────────────────────────────────

TEST(Iterator, empty_memtable_is_invalid) {
    Memtable mem;
    auto it = mem.NewIterator();
    it.SeekToFirst();
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, single_entry_traversal) {
    Memtable mem;
    mem.Put("row", "col", 100, "value");
    auto it = mem.NewIterator();
    it.SeekToFirst();
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.key().row, "row");
    EXPECT_EQ(it.key().col, "col");
    EXPECT_EQ(it.key().timestamp, 100);
    EXPECT_EQ(it.value(), "value");
    it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, row_order_is_ascending) {
    Memtable mem;
    mem.Put("charlie", "col", 100, "v3");
    mem.Put("alice",   "col", 100, "v1");
    mem.Put("bob",     "col", 100, "v2");

    auto it = mem.NewIterator();
    it.SeekToFirst();
    EXPECT_EQ(it.key().row, "alice");   it.Next();
    EXPECT_EQ(it.key().row, "bob");     it.Next();
    EXPECT_EQ(it.key().row, "charlie"); it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, timestamp_order_is_descending) {
    Memtable mem;
    mem.Put("row", "col", 100, "v1");
    mem.Put("row", "col", 300, "v3");
    mem.Put("row", "col", 200, "v2");

    auto it = mem.NewIterator();
    it.SeekToFirst();
    EXPECT_EQ(it.key().timestamp, 300); it.Next();
    EXPECT_EQ(it.key().timestamp, 200); it.Next();
    EXPECT_EQ(it.key().timestamp, 100); it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, tombstones_appear_in_iteration) {
    Memtable mem;
    mem.Put("row", "col", 100, "value");
    mem.Delete("row", "col", 200);

    auto it = mem.NewIterator();
    it.SeekToFirst();
    // Tombstone at ts=200 comes first (descending)
    EXPECT_EQ(it.key().timestamp, 200);
    EXPECT_EQ(it.key().type, kTypeDeletion);
    it.Next();
    EXPECT_EQ(it.key().timestamp, 100);
    EXPECT_EQ(it.key().type, kTypeValue);
    it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, seek_lands_on_correct_entry) {
    Memtable mem;
    mem.Put("apple",  "col", 100, "v1");
    mem.Put("banana", "col", 100, "v2");
    mem.Put("cherry", "col", 100, "v3");

    InternalKey target("banana", "col", INT64_MAX, kTypeValue);
    auto it = mem.NewIterator();
    it.Seek(target);
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.key().row, "banana");
}

TEST(Iterator, full_iteration_count) {
    Memtable mem;
    // 3 rows x 2 cols x 2 versions = 12 entries total
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 2; ++c) {
            for (int ts = 1; ts <= 2; ++ts) {
                mem.Put("row" + std::to_string(r),
                        "col" + std::to_string(c),
                        ts, "value");
            }
        }
    }
    auto it = mem.NewIterator();
    int count = 0;
    for (it.SeekToFirst(); it.Valid(); it.Next()) ++count;
    EXPECT_EQ(count, 12);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 6: Size tracking
//
// The SkipList constructor allocates a sentinel head node from the Arena,
// so ApproximateSize() is already non-zero on an empty Memtable.
// The Arena allocates in 4KB blocks — size grows only when a block is
// exhausted, not on every insert.
// ─────────────────────────────────────────────────────────────────────────────

TEST(Size, empty_memtable_has_nonzero_size) {
    // SkipList head sentinel causes one Arena block allocation at construction
    Memtable mem;
    EXPECT_GT(mem.ApproximateSize(), 0u);
}

TEST(Size, size_grows_when_block_exhausted) {
    // Insert enough data to exceed one 4KB Arena block
    Memtable mem;
    size_t initial = mem.ApproximateSize();
    // Each insert stores a SkipList node — insert enough to fill the block
    for (int i = 0; i < 500; ++i) {
        mem.Put("row" + std::to_string(i), "col", i, std::string(10, 'x'));
    }
    EXPECT_GT(mem.ApproximateSize(), initial);
}

TEST(Size, size_never_shrinks) {
    // Arena is a bump allocator — usage can only stay same or grow, never shrink
    Memtable mem;
    size_t prev = mem.ApproximateSize();
    for (int i = 0; i < 100; ++i) {
        mem.Put("row" + std::to_string(i), "col", i, "value");
        size_t curr = mem.ApproximateSize();
        EXPECT_TRUE(curr >= prev);
        prev = curr;
    }
}

TEST(Size, more_inserts_never_decrease_size) {
    Memtable mem;
    for (int i = 0; i < 1000; ++i) {
        mem.Put("row" + std::to_string(i), "col", i, "value");
    }
    size_t size_at_1000 = mem.ApproximateSize();
    for (int i = 1000; i < 1100; ++i) {
        mem.Put("row" + std::to_string(i), "col", i, "value");
    }
    EXPECT_TRUE(mem.ApproximateSize() >= size_at_1000);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 7: Boundary
// ─────────────────────────────────────────────────────────────────────────────

TEST(Boundary, empty_row_and_col) {
    Memtable mem;
    mem.Put("", "", 100, "value");
    std::string out;
    EXPECT_EQ(mem.Get("", "", out), GetResult::kFound);
    EXPECT_EQ(out, "value");
}

TEST(Boundary, int64_max_timestamp) {
    Memtable mem;
    mem.Put("row", "col", INT64_MAX, "newest");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "newest");
}

TEST(Boundary, int64_min_timestamp) {
    Memtable mem;
    mem.Put("row", "col", INT64_MIN, "oldest");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "oldest");
}

TEST(Boundary, int64_max_beats_int64_min) {
    Memtable mem;
    mem.Put("row", "col", INT64_MIN, "oldest");
    mem.Put("row", "col", INT64_MAX, "newest");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "newest");
}

TEST(Boundary, very_long_row_key) {
    Memtable mem;
    std::string long_row(10 * 1024, 'x');
    mem.Put(long_row, "col", 100, "value");
    std::string out;
    EXPECT_EQ(mem.Get(long_row, "col", out), GetResult::kFound);
    EXPECT_EQ(out, "value");
}

TEST(Boundary, negative_timestamp) {
    Memtable mem;
    mem.Put("row", "col", -500, "value");
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "value");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 8: Stress
// ─────────────────────────────────────────────────────────────────────────────

TEST(Stress, many_rows_all_readable) {
    Memtable mem;
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        mem.Put("row" + std::to_string(i), "col", i, "val" + std::to_string(i));
    }
    std::string out;
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(mem.Get("row" + std::to_string(i), "col", out), GetResult::kFound);
        EXPECT_EQ(out, "val" + std::to_string(i));
    }
}

TEST(Stress, many_versions_get_returns_newest) {
    Memtable mem;
    const int N = 500;
    for (int i = 1; i <= N; ++i) {
        mem.Put("row", "col", i, "val" + std::to_string(i));
    }
    std::string out;
    EXPECT_EQ(mem.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "val" + std::to_string(N));
}

TEST(Stress, iterator_count_matches_inserts) {
    Memtable mem;
    const int N = 200;
    for (int i = 0; i < N; ++i) {
        mem.Put("row" + std::to_string(i), "col", 100, "value");
    }
    auto it = mem.NewIterator();
    int count = 0;
    for (it.SeekToFirst(); it.Valid(); it.Next()) ++count;
    EXPECT_EQ(count, N);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "\n=== Memtable Tests ===\n\n";
    if (g_failed == 0) {
        std::cout << "Results : " << g_passed << " / " << g_total << " assertions passed\n";
        std::cout << "ALL TESTS PASSED\n\n";
        return 0;
    }
    std::cout << "Results : " << g_passed << " / " << g_total << " assertions passed\n";
    std::cout << g_failed << " ASSERTION(S) FAILED\n\n";
    return 1;
}
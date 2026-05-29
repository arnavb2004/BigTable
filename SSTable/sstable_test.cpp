#include "sstable.hpp"
#include "../MemTable/memtable.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <map>
#include <random>
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
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static int g_file_counter = 0;

// Returns a unique temp file path and registers it for cleanup.
static std::vector<std::string>& TempFiles() {
    static std::vector<std::string> files;
    return files;
}

static std::string TempDir() {
    // Works on both Windows and Linux.
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    return std::string(tmp);
}

static std::string TempPath() {
    std::string p = TempDir() + "/sstable_test_" + std::to_string(++g_file_counter) + ".sst";
    TempFiles().push_back(p);
    return p;
}

// Write a sorted list of (key, value) pairs to an SSTable and return the path.
static std::string WriteSSTable(const std::vector<std::pair<InternalKey, std::string>>& entries) {
    std::string path = TempPath();
    SSTableWriter w(path);
    for (auto& [k, v] : entries) w.Add(k, v);
    w.Finish();
    return path;
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 1: BloomFilter
// ─────────────────────────────────────────────────────────────────────────────

TEST(Bloom, added_key_always_found) {
    BloomFilter bf(100);
    std::vector<std::string> keys = {"apple", "banana", "cherry", "com.google.www"};
    for (auto& k : keys) bf.Add(k);
    std::string ser = bf.Finish();
    for (auto& k : keys)
        EXPECT_TRUE(BloomFilter::MayContain(ser, k));
}

TEST(Bloom, absent_key_usually_not_found) {
    BloomFilter bf(1000);
    for (int i = 0; i < 1000; ++i) bf.Add("key" + std::to_string(i));
    std::string ser = bf.Finish();

    // Check false positive rate: should be well under 5% for 10 bits/key.
    int false_positives = 0;
    for (int i = 1000; i < 2000; ++i) {
        if (BloomFilter::MayContain(ser, "key" + std::to_string(i)))
            ++false_positives;
    }
    // 1000 absent keys, expect < 50 false positives (~1% target rate).
    EXPECT_LT(false_positives, 50);
}

TEST(Bloom, empty_filter_is_safe) {
    BloomFilter bf(0);
    std::string ser = bf.Finish();
    // Should not crash and should conservatively return true.
    EXPECT_TRUE(BloomFilter::MayContain(ser, "anything"));
}

TEST(Bloom, corrupt_filter_returns_true) {
    // Corrupt serialised filter must not crash and must return true (safe default).
    EXPECT_TRUE(BloomFilter::MayContain("xyz", "key"));
    EXPECT_TRUE(BloomFilter::MayContain("", "key"));
}

TEST(Bloom, serialise_roundtrip_size) {
    // Format: [bitset_size:4B][bitset][num_hashes:1B]
    // For 100 keys at 10 bits/key = 1000 bits = 125 bytes bitset.
    BloomFilter bf(100);
    bf.Add("test");
    std::string ser = bf.Finish();
    // 4 + 125 + 1 = 130 bytes
    EXPECT_EQ(ser.size(), 4u + 125u + 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 2: Footer
// ─────────────────────────────────────────────────────────────────────────────

TEST(Footer, encode_decode_roundtrip) {
    Footer f;
    f.index_offset = 0xDEADBEEF12345678ULL;
    f.index_size   = 0xCAFEBABE;
    f.num_blocks   = 42;
    std::string encoded = f.Encode();
    EXPECT_EQ(encoded.size(), bigtable::kFooterSize);
    Footer out;
    EXPECT_TRUE(Footer::Decode(encoded, out));
    EXPECT_EQ(out.index_offset, f.index_offset);
    EXPECT_EQ(out.index_size,   f.index_size);
    EXPECT_EQ(out.num_blocks,   f.num_blocks);
    EXPECT_EQ(out.magic,        bigtable::kSSTableMagic);
    EXPECT_EQ(out.version,      bigtable::kSSTableVersion);
}

TEST(Footer, wrong_magic_fails_decode) {
    Footer f;
    std::string encoded = f.Encode();
    // Corrupt the magic bytes.
    for (int i = 16; i < 24; ++i) encoded[i] = 0x00;
    Footer out;
    EXPECT_FALSE(Footer::Decode(encoded, out));
}

TEST(Footer, wrong_size_fails_decode) {
    Footer out;
    EXPECT_FALSE(Footer::Decode("tooshort", out));
    EXPECT_FALSE(Footer::Decode("", out));
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 3: Writer — basic
// ─────────────────────────────────────────────────────────────────────────────

TEST(Writer, single_entry_creates_file) {
    std::string path = TempPath();
    SSTableWriter w(path);
    w.Add(InternalKey("row", "col", 100, kTypeValue), "value");
    w.Finish();
    EXPECT_GT(std::filesystem::file_size(path), 0u);
}

TEST(Writer, num_entries_tracked) {
    std::string path = TempPath();
    SSTableWriter w(path);
    for (int i = 0; i < 10; ++i)
        w.Add(InternalKey("row" + std::to_string(i), "col", 100, kTypeValue), "v");
    EXPECT_EQ(w.NumEntries(), 10u);
    w.Finish();
    EXPECT_EQ(w.NumEntries(), 10u);
}

TEST(Writer, file_size_grows_with_entries) {
    std::string path1 = TempPath();
    std::string path2 = TempPath();
    {
        SSTableWriter w(path1);
        w.Add(InternalKey("row", "col", 1, kTypeValue), "v");
        w.Finish();
    }
    {
        SSTableWriter w(path2);
        for (int i = 0; i < 200; ++i)
            w.Add(InternalKey("row" + std::to_string(i), "col", 1, kTypeValue),
                  std::string(100, 'x'));
        w.Finish();
    }
    EXPECT_LT(std::filesystem::file_size(path1),
              std::filesystem::file_size(path2));
}

TEST(Writer, multiple_blocks_created) {
    std::string path = TempPath();
    SSTableWriter w(path);
    // Each entry is ~150 bytes; 4096 / 150 ≈ 27 entries per block.
    // 100 entries should produce multiple blocks.
    for (int i = 0; i < 100; ++i)
        w.Add(InternalKey("row" + std::to_string(i), "col:family", i, kTypeValue),
              std::string(100, 'v'));
    w.Finish();
    EXPECT_GT(w.NumBlocks(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 4: Writer → Reader roundtrip
// ─────────────────────────────────────────────────────────────────────────────

TEST(Roundtrip, single_entry_get_found) {
    auto path = WriteSSTable({
        {InternalKey("row", "col", 100, kTypeValue), "hello"}
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "hello");
}

TEST(Roundtrip, missing_key_not_found) {
    auto path = WriteSSTable({
        {InternalKey("row", "col", 100, kTypeValue), "hello"}
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("other", "col", out), GetResult::kNotFound);
    EXPECT_EQ(r.Get("row", "other", out), GetResult::kNotFound);
}

TEST(Roundtrip, deletion_tombstone_returns_deleted) {
    auto path = WriteSSTable({
        {InternalKey("row", "col", 100, kTypeDeletion), ""}
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("row", "col", out), GetResult::kDeleted);
}

TEST(Roundtrip, newest_version_returned) {
    // Multiple versions of same (row, col) — newest (highest ts) must come first.
    auto path = WriteSSTable({
        {InternalKey("row", "col", 300, kTypeValue), "v3"},  // sorted: 300 first
        {InternalKey("row", "col", 200, kTypeValue), "v2"},
        {InternalKey("row", "col", 100, kTypeValue), "v1"},
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "v3");
}

TEST(Roundtrip, tombstone_shadows_older_value) {
    auto path = WriteSSTable({
        {InternalKey("row", "col", 200, kTypeDeletion), ""},  // newer
        {InternalKey("row", "col", 100, kTypeValue),    "v"}, // older
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("row", "col", out), GetResult::kDeleted);
}

TEST(Roundtrip, multiple_rows_all_found) {
    std::vector<std::pair<InternalKey, std::string>> entries;
    // Must be in sorted order: row ASC
    for (int i = 0; i < 20; ++i)
        entries.push_back({InternalKey("row" + std::to_string(i), "col", 100, kTypeValue),
                           "val" + std::to_string(i)});
    // Sort properly
    InternalKeyComparator cmp;
    std::sort(entries.begin(), entries.end(),
              [&](auto& a, auto& b){ return cmp(a.first, b.first) < 0; });

    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    std::string out;
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(r.Get("row" + std::to_string(i), "col", out), GetResult::kFound);
        EXPECT_EQ(out, "val" + std::to_string(i));
    }
}

TEST(Roundtrip, bigtable_style_column_families) {
    auto path = WriteSSTable({
        {InternalKey("com.google.www", "anchor:cnnsi",   100, kTypeValue), "CNN"},
        {InternalKey("com.google.www", "contents:html",  100, kTypeValue), "<html>"},
        {InternalKey("com.google.www", "contents:size",  100, kTypeValue), "1024"},
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("com.google.www", "anchor:cnnsi",  out), GetResult::kFound);
    EXPECT_EQ(out, "CNN");
    EXPECT_EQ(r.Get("com.google.www", "contents:html", out), GetResult::kFound);
    EXPECT_EQ(out, "<html>");
    EXPECT_EQ(r.Get("com.google.www", "contents:size", out), GetResult::kFound);
    EXPECT_EQ(out, "1024");
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 5: Metadata
// ─────────────────────────────────────────────────────────────────────────────

TEST(Metadata, smallest_and_largest_keys) {
    auto path = WriteSSTable({
        {InternalKey("apple",  "col", 100, kTypeValue), "a"},
        {InternalKey("banana", "col", 100, kTypeValue), "b"},
        {InternalKey("cherry", "col", 100, kTypeValue), "c"},
    });
    SSTableReader r(path);
    EXPECT_EQ(r.SmallestKey().row, "apple");
    EXPECT_EQ(r.LargestKey().row,  "cherry");
}

TEST(Metadata, file_size_nonzero) {
    auto path = WriteSSTable({
        {InternalKey("row", "col", 1, kTypeValue), "v"}
    });
    SSTableReader r(path);
    EXPECT_GT(r.FileSize(), 0u);
}

TEST(Metadata, num_blocks_correct) {
    std::string path = TempPath();
    SSTableWriter w(path);
    // Write enough to guarantee >1 block
    for (int i = 0; i < 200; ++i)
        w.Add(InternalKey("row" + std::to_string(i), "col", i, kTypeValue),
              std::string(50, 'x'));
    w.Finish();
    SSTableReader r(path);
    EXPECT_EQ(r.NumBlocks(), w.NumBlocks());
    EXPECT_GT(r.NumBlocks(), 1u);
}

TEST(Metadata, may_contain_present_key) {
    auto path = WriteSSTable({
        {InternalKey("row", "col", 100, kTypeValue), "v"}
    });
    SSTableReader r(path);
    EXPECT_TRUE(r.MayContain("row", "col"));
}

TEST(Metadata, may_contain_absent_row_false) {
    auto path = WriteSSTable({
        {InternalKey("bbb", "col", 100, kTypeValue), "v"}
    });
    SSTableReader r(path);
    // "aaa" < smallest key "bbb" → definitely not present
    EXPECT_FALSE(r.MayContain("aaa", "col"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 6: Iterator
// ─────────────────────────────────────────────────────────────────────────────

TEST(Iterator, forward_scan_matches_written_order) {
    std::vector<std::pair<InternalKey, std::string>> entries = {
        {InternalKey("a", "col", 100, kTypeValue), "va"},
        {InternalKey("b", "col", 100, kTypeValue), "vb"},
        {InternalKey("c", "col", 100, kTypeValue), "vc"},
    };
    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    auto it = r.NewIterator();
    it.SeekToFirst();

    for (auto& [k, v] : entries) {
        EXPECT_TRUE(it.Valid());
        EXPECT_EQ(it.key().row, k.row);
        EXPECT_EQ(it.value(), v);
        it.Next();
    }
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, seek_lands_on_correct_key) {
    auto path = WriteSSTable({
        {InternalKey("apple",  "col", 100, kTypeValue), "a"},
        {InternalKey("banana", "col", 100, kTypeValue), "b"},
        {InternalKey("cherry", "col", 100, kTypeValue), "c"},
    });
    SSTableReader r(path);
    auto it = r.NewIterator();
    it.Seek(InternalKey("banana", "col", INT64_MAX, kTypeValue));
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.key().row, "banana");
}

TEST(Iterator, seek_past_end_is_invalid) {
    auto path = WriteSSTable({
        {InternalKey("aaa", "col", 100, kTypeValue), "v"},
    });
    SSTableReader r(path);
    auto it = r.NewIterator();
    it.Seek(InternalKey("zzz", "col", INT64_MAX, kTypeValue));
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, count_matches_written_entries) {
    const int N = 50;
    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int i = 0; i < N; ++i)
        entries.push_back({InternalKey("row" + std::to_string(i), "col", i, kTypeValue), "v"});
    InternalKeyComparator cmp;
    std::sort(entries.begin(), entries.end(),
              [&](auto& a, auto& b){ return cmp(a.first, b.first) < 0; });

    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    auto it = r.NewIterator();
    int count = 0;
    for (it.SeekToFirst(); it.Valid(); it.Next()) ++count;
    EXPECT_EQ(count, N);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 7: Memtable flush → SSTable roundtrip
//
// This is the most important integration test: simulates what minor compaction
// does — freeze Memtable, iterate it, flush to SSTable, read back.
// ─────────────────────────────────────────────────────────────────────────────

TEST(Integration, memtable_flush_roundtrip) {
    // Build a Memtable with several rows and versions.
    Memtable mem;
    mem.Put("com.google.www", "contents:html",  300, "<html v3>");
    mem.Put("com.google.www", "contents:html",  200, "<html v2>");
    mem.Put("com.google.www", "contents:html",  100, "<html v1>");
    mem.Put("com.google.www", "anchor:cnnsi",   100, "CNN");
    mem.Put("com.apple.www",  "contents:html",  100, "Apple");
    mem.Delete("com.apple.www", "contents:html", 200);  // tombstone

    // Flush memtable → SSTable.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        auto it = mem.NewIterator();
        for (it.SeekToFirst(); it.Valid(); it.Next())
            w.Add(it.key(), it.value());
        w.Finish();
    }

    // Read back and verify.
    SSTableReader r(path);
    std::string out;

    // Newest version of com.google.www/contents:html is ts=300.
    EXPECT_EQ(r.Get("com.google.www", "contents:html", out), GetResult::kFound);
    EXPECT_EQ(out, "<html v3>");

    EXPECT_EQ(r.Get("com.google.www", "anchor:cnnsi", out), GetResult::kFound);
    EXPECT_EQ(out, "CNN");

    // com.apple.www was deleted at ts=200 (newer than the Put at ts=100).
    EXPECT_EQ(r.Get("com.apple.www", "contents:html", out), GetResult::kDeleted);

    // Missing key.
    EXPECT_EQ(r.Get("com.missing.www", "col", out), GetResult::kNotFound);
}

TEST(Integration, large_memtable_flush_all_keys_readable) {
    const int N = 500;
    Memtable mem;
    for (int i = 0; i < N; ++i)
        mem.Put("row" + std::to_string(i), "col:data", i, "val" + std::to_string(i));

    std::string path = TempPath();
    {
        SSTableWriter w(path);
        auto it = mem.NewIterator();
        for (it.SeekToFirst(); it.Valid(); it.Next())
            w.Add(it.key(), it.value());
        w.Finish();
    }

    SSTableReader r(path);
    std::string out;
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(r.Get("row" + std::to_string(i), "col:data", out), GetResult::kFound);
        EXPECT_EQ(out, "val" + std::to_string(i));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 8: Boundary
// ─────────────────────────────────────────────────────────────────────────────

TEST(Boundary, empty_row_and_col) {
    auto path = WriteSSTable({
        {InternalKey("", "", 100, kTypeValue), "empty_key_value"}
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("", "", out), GetResult::kFound);
    EXPECT_EQ(out, "empty_key_value");
}

TEST(Boundary, int64_max_timestamp) {
    auto path = WriteSSTable({
        {InternalKey("row", "col", INT64_MAX, kTypeValue), "newest"}
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "newest");
}

TEST(Boundary, empty_value) {
    auto path = WriteSSTable({
        {InternalKey("row", "col", 1, kTypeValue), ""}
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "");
}

TEST(Boundary, large_value) {
    std::string big(64 * 1024, 'z');  // 64KB
    auto path = WriteSSTable({
        {InternalKey("row", "col", 1, kTypeValue), big}
    });
    SSTableReader r(path);
    std::string out;
    EXPECT_EQ(r.Get("row", "col", out), GetResult::kFound);
    EXPECT_EQ(out, big);
}

TEST(Boundary, corrupt_footer_throws) {
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        w.Add(InternalKey("row", "col", 1, kTypeValue), "v");
        w.Finish();
    }
    // Corrupt the last 28 bytes (footer).
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(-static_cast<int>(bigtable::kFooterSize), std::ios::end);
        char zeros[bigtable::kFooterSize] = {};
        f.write(zeros, bigtable::kFooterSize);
    }
    bool threw = false;
    try { SSTableReader r(path); } catch (const std::runtime_error&) { threw = true; }
    EXPECT_TRUE(threw);
}

// ─────────────────────────────────────────────────────────────────────────────
// Suite 9: Ground-truth stress
//
// Random Put/Delete ops against a Memtable, flush to SSTable, verify every
// Get against the ground-truth std::map.
// ─────────────────────────────────────────────────────────────────────────────

TEST(GroundTruth, random_ops_match_map) {
    Memtable mem;
    struct State { std::string value; bool deleted = false; };
    std::map<std::pair<std::string,std::string>, State> truth;

    std::mt19937 rng(42);
    auto ri = [&](int lo, int hi){ return std::uniform_int_distribution<int>(lo,hi)(rng); };

    const int ROWS = 30, COLS = 5, OPS = 1000;
    int64_t ts = 1;

    for (int op = 0; op < OPS; ++op) {
        std::string row = "row" + std::to_string(ri(0, ROWS-1));
        std::string col = "col" + std::to_string(ri(0, COLS-1));
        auto key = std::make_pair(row, col);
        if (ri(0,1) == 0) {
            std::string val = "v" + std::to_string(op);
            mem.Put(row, col, ts++, val);
            truth[key] = {val, false};
        } else {
            mem.Delete(row, col, ts++);
            truth[key] = {"", true};
        }
    }

    // Flush to SSTable.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        auto it = mem.NewIterator();
        for (it.SeekToFirst(); it.Valid(); it.Next())
            w.Add(it.key(), it.value());
        w.Finish();
    }

    // Verify every entry.
    SSTableReader r(path);
    std::string out;
    for (auto& [key, state] : truth) {
        GetResult result = r.Get(key.first, key.second, out);
        if (state.deleted) {
            EXPECT_EQ(result, GetResult::kDeleted);
        } else {
            EXPECT_EQ(result, GetResult::kFound);
            EXPECT_EQ(out, state.value);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
TEST(IteratorBoundary, unaligned_block_boundary_smoke) {
    const int kTotalEntries = 50;
    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int i = 0; i < kTotalEntries; ++i) {
        char row[7]; std::snprintf(row, sizeof(row), "r%05d", i);
        size_t variable_len = (i % 2 == 0) ? 300 : 700;
        entries.push_back({
            InternalKey(row, "col:f1", static_cast<int64_t>(1000 - i), kTypeValue),
            std::string(variable_len, static_cast<char>('a' + (i % 26)))
        });
    }
    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    EXPECT_GT(r.NumBlocks(), 1u);
    auto it = r.NewIterator();
    it.SeekToFirst();
    for (int i = 0; i < kTotalEntries; ++i) {
        EXPECT_TRUE(it.Valid());
        if (!it.Valid()) break;
        char expected[7]; std::snprintf(expected, sizeof(expected), "r%05d", i);
        EXPECT_EQ(it.key().row, std::string(expected));
        it.Next();
    }
    EXPECT_FALSE(it.Valid());
}

// =============================================================================
// Suite 14: Production-Grade File Validation
//
// Tests for the three hardening fixes:
//   1. streamoff overflow guard in ReadAt
//   2. Footer version field validation
//   3. num_blocks sanity cap against corrupt index
// Each test injects a specific corruption and verifies a std::runtime_error
// is thrown with a meaningful message rather than silently misbehaving.
// =============================================================================

// Helper: write a valid single-entry SSTable and return its raw bytes.
static std::string ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

static void WriteFileBytes(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Patch `count` bytes at `offset` within a file with `fill`.
// Write a big-endian uint32 into bytes at offset.
static void PatchU32BE(std::string& bytes, size_t offset, uint32_t value) {
    bytes[offset]   = static_cast<char>((value >> 24) & 0xFF);
    bytes[offset+1] = static_cast<char>((value >> 16) & 0xFF);
    bytes[offset+2] = static_cast<char>((value >>  8) & 0xFF);
    bytes[offset+3] = static_cast<char>( value        & 0xFF);
}

// ── 1. Footer version mismatch ────────────────────────────────────────────────

TEST(FileValidation, wrong_footer_version_throws) {
    // Write a valid SSTable, then corrupt the version field in the footer.
    // Footer layout (last 28 bytes):
    //   [index_offset:8][index_size:4][num_blocks:4][magic:8][version:4]
    // version is at offset -4 from end of file.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        w.Add(InternalKey("row", "col", 1, kTypeValue), "v");
        w.Finish();
    }

    std::string bytes = ReadFileBytes(path);
    // Overwrite version field (last 4 bytes) with version=99.
    PatchU32BE(bytes, bytes.size() - 4, 99u);
    WriteFileBytes(path, bytes);

    bool threw = false;
    std::string msg;
    try {
        SSTableReader r(path);
    } catch (const std::runtime_error& e) {
        threw = true;
        msg   = e.what();
    }
    EXPECT_TRUE(threw);
    // Message must mention "version" so the operator knows what went wrong.
    EXPECT_TRUE(msg.find("version") != std::string::npos);
}

TEST(FileValidation, correct_version_does_not_throw) {
    // Sanity: a valid SSTable with the correct version opens without error.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        w.Add(InternalKey("row", "col", 1, kTypeValue), "v");
        w.Finish();
    }
    bool threw = false;
    try { SSTableReader r(path); } catch (...) { threw = true; }
    EXPECT_FALSE(threw);
}

// ── 2. num_blocks sanity cap ──────────────────────────────────────────────────

TEST(FileValidation, implausible_block_count_throws) {
    // Corrupt the num_blocks field inside the index block to a value far
    // exceeding kMaxSaneBlocks (16384). The reader must throw rather than
    // trying to reserve() billions of BlockHandles.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        w.Add(InternalKey("row", "col", 1, kTypeValue), "v");
        w.Finish();
    }

    // The index block starts at footer.index_offset.
    // Read the footer to find index_offset.
    std::string bytes = ReadFileBytes(path);
    size_t footer_start = bytes.size() - bigtable::kFooterSize;
    // index_offset is first 8 bytes of footer — decode big-endian.
    uint64_t index_offset = 0;
    for (int i = 0; i < 8; ++i)
        index_offset = (index_offset << 8) |
                       static_cast<uint8_t>(bytes[footer_start + i]);

    // First 4 bytes of the index block = num_blocks. Overwrite with 0xFFFFFFFF.
    PatchU32BE(bytes, static_cast<size_t>(index_offset), 0xFFFFFFFFu);
    WriteFileBytes(path, bytes);

    bool threw = false;
    std::string msg;
    try {
        SSTableReader r(path);
    } catch (const std::runtime_error& e) {
        threw = true;
        msg   = e.what();
    }
    EXPECT_TRUE(threw);
    EXPECT_TRUE(msg.find("block") != std::string::npos ||
                msg.find("implausible") != std::string::npos);
}

TEST(FileValidation, plausible_block_count_does_not_throw) {
    // A multi-block file with a legitimate block count opens correctly.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        for (int i = 0; i < 200; ++i) {
            char row[8]; std::snprintf(row, sizeof(row), "r%06d", i);
            w.Add(InternalKey(row, "col", i, kTypeValue), std::string(30, 'x'));
        }
        w.Finish();
        EXPECT_GT(w.NumBlocks(), 1u);
    }
    bool threw = false;
    try { SSTableReader r(path); } catch (...) { threw = true; }
    EXPECT_FALSE(threw);
}

// ── 3. Index block bounds validation ──────────────────────────────────────────

TEST(FileValidation, index_offset_past_footer_throws) {
    // Set index_offset in the footer to point past the footer start.
    // The reader must detect this before attempting to ReadAt() garbage.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        w.Add(InternalKey("row", "col", 1, kTypeValue), "v");
        w.Finish();
    }
    std::string bytes = ReadFileBytes(path);
    size_t footer_start = bytes.size() - bigtable::kFooterSize;

    // Overwrite index_offset (first 8 bytes of footer) with file_size itself
    // (one byte past valid range).
    uint64_t bad_offset = static_cast<uint64_t>(bytes.size());
    for (int i = 7; i >= 0; --i) {
        bytes[footer_start + (7 - i)] = static_cast<char>((bad_offset >> (8*i)) & 0xFF);
    }
    WriteFileBytes(path, bytes);

    bool threw = false;
    try { SSTableReader r(path); } catch (const std::runtime_error&) { threw = true; }
    EXPECT_TRUE(threw);
}

TEST(FileValidation, data_block_offset_outside_file_throws) {
    // Corrupt a BlockHandle in the index so its data block offset is past EOF.
    // The per-entry bounds check must catch this during index parsing.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        w.Add(InternalKey("row", "col", 1, kTypeValue), "v");
        w.Finish();
    }
    std::string bytes = ReadFileBytes(path);
    size_t footer_start = bytes.size() - bigtable::kFooterSize;

    // Find index_offset.
    uint64_t index_offset = 0;
    for (int i = 0; i < 8; ++i)
        index_offset = (index_offset << 8) |
                       static_cast<uint8_t>(bytes[footer_start + i]);

    // Index layout: [num_blocks:4][key_size:4][key...][offset:8][size:4]...
    // Overwrite the block offset (after num_blocks(4) + key_size(4) + key)
    // with a value larger than the file.
    // First: read key_size to know where offset starts.
    size_t idx = static_cast<size_t>(index_offset);
    idx += 4; // skip num_blocks
    uint32_t key_size = (static_cast<uint8_t>(bytes[idx])   << 24)
                      | (static_cast<uint8_t>(bytes[idx+1]) << 16)
                      | (static_cast<uint8_t>(bytes[idx+2]) <<  8)
                      |  static_cast<uint8_t>(bytes[idx+3]);
    idx += 4 + key_size; // skip key_size field + key bytes

    // idx now points at block offset (8 bytes). Write 0xFFFFFFFFFFFFFFFF.
    for (int i = 0; i < 8; ++i)
        bytes[idx + i] = static_cast<char>(0xFF);
    WriteFileBytes(path, bytes);

    bool threw = false;
    try { SSTableReader r(path); } catch (const std::runtime_error&) { threw = true; }
    EXPECT_TRUE(threw);
}

// ── 4. shared > current_key.size() guard ──────────────────────────────────────

TEST(FileValidation, corrupt_shared_prefix_throws) {
    // Corrupt a data block entry's `shared` field to exceed the current key
    // length. The reader must throw rather than calling resize() with an
    // out-of-range value that produces garbage.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        // Two entries so the second one has a valid shared prefix to corrupt.
        w.Add(InternalKey("aaa", "col", 200, kTypeValue), "v1");
        w.Add(InternalKey("aab", "col", 100, kTypeValue), "v2");
        w.Finish();
    }
    std::string bytes = ReadFileBytes(path);

    // The data block starts at offset 0 (first block, offset field = 0).
    // Entry format: [shared:4][unshared:4][val_len:4][key_delta][value]
    // First entry: shared=0. Skip it (12 + encoded_key_size + val_size).
    // We need to find the second entry and corrupt its shared field.
    // Rather than parsing precisely, just scan for the second entry header
    // by finding the first non-zero shared value and setting it to 0xFF.
    // Actually: patch a known-small offset in the block.
    // The first entry's shared is 0 (restart point).
    // Find byte offset 0 in the block — that's the block start.
    // First entry header: [0:4][unshared:4][val_len:4]
    // Skip first entry: 12 + unshared + val_len bytes
    // Second entry starts there; its [shared:4] = actual shared prefix len.
    // We just write 0xFFFFFFFF there.

    // Parse first entry header to skip it.
    size_t pos = 0;
    pos += 4;  // skip shared field of first entry (always 0 at restart point)
    uint32_t ush = (static_cast<uint8_t>(bytes[pos])   << 24) |
                   (static_cast<uint8_t>(bytes[pos+1]) << 16) |
                   (static_cast<uint8_t>(bytes[pos+2]) <<  8) |
                    static_cast<uint8_t>(bytes[pos+3]);   pos += 4;
    uint32_t vl  = (static_cast<uint8_t>(bytes[pos])   << 24) |
                   (static_cast<uint8_t>(bytes[pos+1]) << 16) |
                   (static_cast<uint8_t>(bytes[pos+2]) <<  8) |
                    static_cast<uint8_t>(bytes[pos+3]);   pos += 4;
    pos += ush + vl;  // skip key_delta + value of first entry

    // pos now points at second entry's shared field. Overwrite with huge value.
    PatchU32BE(bytes, pos, 0xFFFFFFFFu);
    WriteFileBytes(path, bytes);

    // Reading the data block must throw, not silently produce garbage keys.
    bool threw = false;
    try {
        SSTableReader r(path);
        // Force ReadDataBlock by iterating.
        auto it = r.NewIterator();
        it.SeekToFirst();
        while (it.Valid()) it.Next();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// ── 5. Partial write detection ────────────────────────────────────────────────

TEST(FileValidation, truncated_file_throws_on_open) {
    // Truncate the file after writing, simulating a crash mid-write.
    // The reader must detect the footer is missing and throw.
    std::string path = TempPath();
    {
        SSTableWriter w(path);
        w.Add(InternalKey("row", "col", 1, kTypeValue), "v");
        w.Finish();
    }
    // Truncate to just 10 bytes — too small for even a footer.
    std::string bytes = ReadFileBytes(path);
    bytes.resize(10);
    WriteFileBytes(path, bytes);

    bool threw = false;
    try { SSTableReader r(path); } catch (const std::runtime_error&) { threw = true; }
    EXPECT_TRUE(threw);
}

TEST(FileValidation, empty_file_throws_on_open) {
    std::string path = TempPath();
    WriteFileBytes(path, "");

    bool threw = false;
    try { SSTableReader r(path); } catch (const std::runtime_error&) { threw = true; }
    EXPECT_TRUE(threw);
}

int main() {
    std::cout << "\n=== SSTable Tests ===\n\n";

    if (g_failed == 0) {
        std::cout << "Results : " << g_passed << " / " << g_total << " assertions passed\n";
        std::cout << "ALL TESTS PASSED\n\n";
    } else {
        std::cout << "Results : " << g_passed << " / " << g_total << " assertions passed\n";
        std::cout << g_failed << " ASSERTION(S) FAILED\n\n";
    }

    // Cleanup temp files.
    for (auto& f : TempFiles())
        std::filesystem::remove(f);

    return g_failed == 0 ? 0 : 1;
}

// =============================================================================
// Suite 10: Iterator Boundary Transitions
//
// We size entries so that exactly kRestartInterval (8) entries fill one 4KB
// block. The 9th entry therefore starts a new block AND is a restart point
// (entry_count_ % kRestartInterval == 0).  Crossing this seam exercises:
//   - Iterator::Next() transitioning from block N to block N+1
//   - The new block being decoded from a full-key restart point (shared=0)
//   - No state bleed from the previous block's current_key into the new one
// =============================================================================

TEST(IteratorBoundary, next_decodes_correctly_across_restart_aligned_block_boundary) {
    // Each entry: row=6B, col=6B → encoded key = 17+6+6 = 29B
    // At restart point (shared=0): wire entry = 12 + 29 + value_len
    // We need 8 entries to exactly fill 4096 bytes: 4096/8 = 512 bytes/entry
    // value_len = 512 - 12 - 29 = 471
    const int    kEntriesPerBlock = 8;
    const size_t kValueLen        = 471;
    const int    kTotalEntries    = kEntriesPerBlock * 3;  // 3 full blocks

    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int i = 0; i < kTotalEntries; ++i) {
        // Zero-padded row so lexicographic order == insertion order
        char row[7]; std::snprintf(row, sizeof(row), "r%05d", i);
        entries.push_back({
            InternalKey(row, "col:f1", static_cast<int64_t>(1000 - i), kTypeValue),
            std::string(kValueLen, static_cast<char>('a' + (i % 26)))
        });
    }
    // Already in sorted order (row ascending, ts descending within same row).

    auto path = WriteSSTable(entries);
    SSTableReader r(path);

    // Must have produced at least 2 blocks (likely 3).
    EXPECT_GT(r.NumBlocks(), 1u);

    // Full forward scan: every entry must decode correctly, in order.
    auto it = r.NewIterator();
    it.SeekToFirst();
    for (int i = 0; i < kTotalEntries; ++i) {
        EXPECT_TRUE(it.Valid());
        if (!it.Valid()) break;

        char expected_row[7]; std::snprintf(expected_row, sizeof(expected_row), "r%05d", i);
        EXPECT_EQ(it.key().row, std::string(expected_row));
        EXPECT_EQ(it.key().col, "col:f1");
        EXPECT_EQ(it.key().timestamp, static_cast<int64_t>(1000 - i));
        // Value must be the correct fill character — not bleed from prior block.
        EXPECT_EQ(it.value(), std::string(kValueLen, static_cast<char>('a' + (i % 26))));
        it.Next();
    }
    EXPECT_FALSE(it.Valid());
}

TEST(IteratorBoundary, value_at_block_boundary_is_not_corrupted_by_previous_block_state) {
    // Two entries: one large enough to fill a block by itself,
    // the next entry starts the second block at a restart point.
    // The second entry's value must not contain bytes from the first block.
    const size_t kBigValue = bigtable::kSSTableBlockSize + 100;

    auto path = WriteSSTable({
        {InternalKey("aaa", "col", 100, kTypeValue), std::string(kBigValue, 'X')},
        {InternalKey("bbb", "col", 100, kTypeValue), std::string(50,       'Y')},
        {InternalKey("ccc", "col", 100, kTypeValue), std::string(50,       'Z')},
    });

    SSTableReader r(path);
    EXPECT_GT(r.NumBlocks(), 1u);

    auto it = r.NewIterator();
    it.SeekToFirst();

    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.key().row, "aaa");
    EXPECT_EQ(it.value().size(), kBigValue);
    EXPECT_TRUE(it.value() == std::string(kBigValue, 'X'));
    it.Next();

    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.key().row, "bbb");
    EXPECT_EQ(it.value().size(), 50u);
    // Every byte must be 'Y', not 'X' leaked from the previous block.
    EXPECT_TRUE(it.value() == std::string(50, 'Y'));
    it.Next();

    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.key().row, "ccc");
    EXPECT_TRUE(it.value() == std::string(50, 'Z'));
    it.Next();

    EXPECT_FALSE(it.Valid());
}

TEST(IteratorBoundary, repeated_next_across_three_blocks_stays_ascending) {
    // Write 3 blocks worth of data, do a full scan, verify strict ascending order.
    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int i = 0; i < 300; ++i) {
        char row[8]; std::snprintf(row, sizeof(row), "r%06d", i);
        entries.push_back({
            InternalKey(row, "c", i, kTypeValue),
            std::string(20, static_cast<char>('a' + i % 26))
        });
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    EXPECT_GT(r.NumBlocks(), 2u);

    auto it = r.NewIterator();
    it.SeekToFirst();

    int prev_i = -1;
    int count  = 0;
    while (it.Valid()) {
        // Timestamp should be exactly equal to loop index (stored as i).
        int cur_i = static_cast<int>(it.key().timestamp);
        EXPECT_GT(cur_i, prev_i);   // strictly ascending index = ascending row
        prev_i = cur_i;
        ++count;
        it.Next();
    }
    EXPECT_EQ(count, 300);
}


// =============================================================================
// Suite 11: Multi-Block Seek Precision
//
// Tests Iterator::Seek() in three gap cases:
//   A. Target is smaller than everything in the file → lands on first entry
//   B. Target falls in the gap between two blocks → lands on first entry of
//      the next block (not past end, not on wrong entry)
//   C. Target is larger than everything → iterator is invalid
// =============================================================================

TEST(SeekPrecision, seek_before_minimum_lands_on_first_entry) {
    // File contains rows "mmm", "nnn", "ppp" (across multiple blocks if needed).
    // Seeking to "aaa" (before "mmm") must land on "mmm".
    auto path = WriteSSTable({
        {InternalKey("mmm", "col", 100, kTypeValue), "v_mmm"},
        {InternalKey("nnn", "col", 100, kTypeValue), "v_nnn"},
        {InternalKey("ppp", "col", 100, kTypeValue), "v_ppp"},
    });

    SSTableReader r(path);
    auto it = r.NewIterator();

    it.Seek(InternalKey("aaa", "col", INT64_MAX, kTypeValue));
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.key().row, "mmm");
    EXPECT_EQ(it.value(), "v_mmm");
}

TEST(SeekPrecision, seek_between_blocks_lands_on_first_entry_of_next_block) {
    // Build two blocks with a clear row gap between them.
    // Block 0: rows "b000000" .. "b000NNN"  (large values force flush)
    // Block 1: rows "d000000" .. "d000MMM"
    // Seek to "c000000" — lexicographically between the two blocks.
    // Must land on the first entry of block 1 ("d000000").

    const size_t kVal = bigtable::kSSTableBlockSize / 2 + 1;  // 2 entries fill a block
    std::vector<std::pair<InternalKey, std::string>> entries;

    // Block 0: 3 large entries starting with 'b'
    for (int i = 0; i < 3; ++i) {
        char row[8]; std::snprintf(row, sizeof(row), "b%06d", i);
        entries.push_back({InternalKey(row, "col", 100, kTypeValue), std::string(kVal, 'b')});
    }
    // Block 1: 3 entries starting with 'd' — guaranteed lexicographically after 'c'
    for (int i = 0; i < 3; ++i) {
        char row[8]; std::snprintf(row, sizeof(row), "d%06d", i);
        entries.push_back({InternalKey(row, "col", 100, kTypeValue), std::string(50, 'd')});
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    EXPECT_GT(r.NumBlocks(), 1u);

    auto it = r.NewIterator();
    // "c000000" is lexicographically between "b..." and "d..."
    it.Seek(InternalKey("c000000", "col", INT64_MAX, kTypeValue));

    EXPECT_TRUE(it.Valid());
    // Must land on first 'd' entry, not past end and not on a 'b' entry.
    EXPECT_EQ(it.key().row[0], 'd');
    EXPECT_EQ(it.key().row, "d000000");
}

TEST(SeekPrecision, seek_exactly_to_first_entry_of_second_block) {
    // Seek to a key that is exactly the first entry of block 1.
    // FindBlock must return block 1, linear scan stops at entry 0.
    const size_t kVal = bigtable::kSSTableBlockSize / 2 + 1;
    std::vector<std::pair<InternalKey, std::string>> entries;

    for (int i = 0; i < 3; ++i) {
        char row[8]; std::snprintf(row, sizeof(row), "b%06d", i);
        entries.push_back({InternalKey(row, "col", 100, kTypeValue), std::string(kVal, 'x')});
    }
    for (int i = 0; i < 3; ++i) {
        char row[8]; std::snprintf(row, sizeof(row), "d%06d", i);
        entries.push_back({InternalKey(row, "col", 100, kTypeValue), std::string(50, 'y')});
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    EXPECT_GT(r.NumBlocks(), 1u);

    auto it = r.NewIterator();
    // Seek to exactly the first key of block 1 (ts=100, same as stored).
    it.Seek(InternalKey("d000000", "col", 100, kTypeValue));
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.key().row, "d000000");
    EXPECT_EQ(it.key().timestamp, 100);
}

TEST(SeekPrecision, seek_past_maximum_is_invalid) {
    auto path = WriteSSTable({
        {InternalKey("aaa", "col", 100, kTypeValue), "v1"},
        {InternalKey("bbb", "col", 100, kTypeValue), "v2"},
    });
    SSTableReader r(path);
    auto it = r.NewIterator();

    it.Seek(InternalKey("zzz", "col", INT64_MAX, kTypeValue));
    EXPECT_FALSE(it.Valid());
}

TEST(SeekPrecision, seek_then_next_walks_remaining_entries_correctly) {
    // After seeking into the middle of the file, Next() must continue
    // reading from the correct position without re-reading earlier entries.
    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int i = 0; i < 200; ++i) {
        char row[8]; std::snprintf(row, sizeof(row), "r%06d", i);
        entries.push_back({InternalKey(row, "col", i, kTypeValue), std::to_string(i)});
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    auto it = r.NewIterator();

    // Seek to entry 100 (row "r000100").
    it.Seek(InternalKey("r000100", "col", INT64_MAX, kTypeValue));
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.key().row, "r000100");

    // Walk the remaining 100 entries and verify each one.
    for (int i = 100; i < 200; ++i) {
        EXPECT_TRUE(it.Valid());
        char expected[8]; std::snprintf(expected, sizeof(expected), "r%06d", i);
        EXPECT_EQ(it.key().row, std::string(expected));
        EXPECT_EQ(it.value(), std::to_string(i));
        it.Next();
    }
    EXPECT_FALSE(it.Valid());
}


// =============================================================================
// Suite 12: Cross-Block Version Splits
//
// Many versions of the same (row, col) user key, with strictly decreasing
// timestamps, written in the order the Memtable iterator produces them
// (ts descending). Enough versions to spill across multiple data blocks.
// SSTableReader::Get() must always return the newest version (highest ts),
// which is the first one written and therefore lives in block 0.
// =============================================================================

TEST(CrossBlockVersions, get_returns_newest_when_versions_span_two_blocks) {
    // Write enough versions to guarantee they cross a block boundary.
    // Each version: same row/col, ts decreasing from N down to 1.
    // Value encodes the timestamp so we can verify which version was returned.
    const int    kVersions = 200;
    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int ts = kVersions; ts >= 1; --ts) {
        // Descending timestamps = ascending InternalKey order (correct SSTable order)
        entries.push_back({
            InternalKey("hotrow", "col:data", static_cast<int64_t>(ts), kTypeValue),
            "ts=" + std::to_string(ts)
        });
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    EXPECT_GT(r.NumBlocks(), 1u);  // confirm versions actually span blocks

    std::string out;
    EXPECT_EQ(r.Get("hotrow", "col:data", out), GetResult::kFound);
    EXPECT_EQ(out, "ts=" + std::to_string(kVersions));  // must be newest
}

TEST(CrossBlockVersions, get_returns_newest_when_versions_span_three_blocks) {
    const int    kVersions = 600;
    const size_t kVal      = 20;

    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int ts = kVersions; ts >= 1; --ts) {
        entries.push_back({
            InternalKey("samerow", "col:v", static_cast<int64_t>(ts), kTypeValue),
            std::string(kVal, static_cast<char>('0' + (ts % 10)))
        });
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    EXPECT_GT(r.NumBlocks(), 2u);

    std::string out;
    EXPECT_EQ(r.Get("samerow", "col:v", out), GetResult::kFound);
    // Newest version is ts=kVersions, its value fill char = kVersions % 10
    std::string expected(kVal, static_cast<char>('0' + (kVersions % 10)));
    EXPECT_EQ(out, expected);
}

TEST(CrossBlockVersions, tombstone_in_block0_shadows_value_in_block1) {
    // Newest entry for (row,col) is a tombstone in block 0.
    // Older live value is in block 1.
    // Get() must return kDeleted, not kFound.
    const int kVersions = 200;

    std::vector<std::pair<InternalKey, std::string>> entries;
    // ts=kVersions is a tombstone (newest)
    entries.push_back({
        InternalKey("shadowrow", "col", static_cast<int64_t>(kVersions), kTypeDeletion), ""
    });
    // Older live values fill block 1
    for (int ts = kVersions - 1; ts >= 1; --ts) {
        entries.push_back({
            InternalKey("shadowrow", "col", static_cast<int64_t>(ts), kTypeValue),
            "alive_" + std::to_string(ts)
        });
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    EXPECT_GT(r.NumBlocks(), 1u);

    std::string out;
    EXPECT_EQ(r.Get("shadowrow", "col", out), GetResult::kDeleted);
}

TEST(CrossBlockVersions, iterator_visits_all_versions_across_blocks_in_order) {
    // The iterator must visit all versions in strictly descending timestamp order
    // (which is ascending InternalKey order) even when they span multiple blocks.
    const int kVersions = 150;

    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int ts = kVersions; ts >= 1; --ts) {
        entries.push_back({
            InternalKey("vrow", "col", static_cast<int64_t>(ts), kTypeValue),
            "v" + std::to_string(ts)
        });
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);

    auto it = r.NewIterator();
    it.SeekToFirst();

    int expected_ts = kVersions;
    int count = 0;
    while (it.Valid()) {
        EXPECT_EQ(it.key().row, "vrow");
        EXPECT_EQ(it.key().col, "col");
        EXPECT_EQ(it.key().timestamp, static_cast<int64_t>(expected_ts));
        EXPECT_EQ(it.value(), "v" + std::to_string(expected_ts));
        --expected_ts;
        ++count;
        it.Next();
    }
    EXPECT_EQ(count, kVersions);
    EXPECT_EQ(expected_ts, 0);
}


// =============================================================================
// Suite 13: Delta Compression Stress
//
// Tests the `current_key.resize(shared); current_key.append(...)` decode
// loop in ReadDataBlock() against pathological key sequences that cause:
//   - The shared prefix to oscillate (large→small→large) within a restart group
//   - Shared prefix to shrink to zero mid-group (then recover)
//   - Keys of varying total lengths so resize() must both grow and shrink
//
// Any bleed from a previous iteration's suffix would produce a wrong decoded
// key, causing Decode() to fail or the key to mismatch.
// =============================================================================

TEST(DeltaCompression, oscillating_prefix_length_decodes_correctly) {
    // Keys with a long shared prefix but alternating short/long suffixes.
    // Within a restart group (8 entries), shared prefix oscillates:
    //   key0: "com.google.www/contents:aaa/..."   (restart, full key)
    //   key1: "com.google.www/contents:aab/..."   (large shared with key0)
    //   key2: "com.google.www/contents:b/..."     (smaller shared — suffix shrinks)
    //   key3: "com.google.www/contents:baa/..."   (shared grows back)
    //   key4: "com.google.www/contents:c/..."     (shrinks again)
    //   ...
    // The resize(shared) must truncate precisely, not leave stale bytes.

    const std::string prefix = "com.google.www";
    const std::string col    = "fam:qual";
    std::vector<std::string> suffixes = {
        "aaa", "aab", "b", "baa", "c", "caa", "d", "daa",   // group 1 (8 entries)
        "eaa", "eab", "f", "faa", "g", "gaa", "h", "haa",   // group 2
        "iaa", "iab", "j", "jaa", "k", "kaa", "l", "laa",   // group 3
    };

    std::vector<std::pair<InternalKey, std::string>> entries;
    for (size_t i = 0; i < suffixes.size(); ++i) {
        std::string row = prefix + "/" + suffixes[i];
        entries.push_back({
            InternalKey(row, col, static_cast<int64_t>(1000 - i), kTypeValue),
            "val_" + suffixes[i]
        });
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);

    // Verify every entry decodes correctly via iterator.
    auto it = r.NewIterator();
    it.SeekToFirst();
    for (size_t i = 0; i < suffixes.size(); ++i) {
        EXPECT_TRUE(it.Valid());
        if (!it.Valid()) break;
        std::string expected_row = prefix + "/" + suffixes[i];
        EXPECT_EQ(it.key().row, expected_row);
        EXPECT_EQ(it.value(), "val_" + suffixes[i]);
        it.Next();
    }
    EXPECT_FALSE(it.Valid());
}

TEST(DeltaCompression, shared_prefix_collapses_to_zero_mid_group) {
    // Within a restart group, force shared prefix to drop to zero mid-group.
    // This happens when consecutive keys share no prefix at all.
    // The resize(0) then append(full_key) path must reconstruct correctly.
    //
    // Keys are chosen so the InternalKeyComparator order is:
    //   aaa_* (col, ts DESC), then zzz_* (col, ts DESC)
    // Entries MUST be added in strictly ascending comparator order.

    // Build entries in correct sorted order: row ASC, col ASC, ts DESC.
    std::vector<std::pair<InternalKey, std::string>> entries = {
        // "aaa" group — same col, timestamps descending
        {InternalKey("aaa_alpha", "col", 800, kTypeValue), "va_800"},
        {InternalKey("aaa_alpha", "col", 700, kTypeValue), "va_700"},
        {InternalKey("aaa_alpha", "col", 600, kTypeValue), "va_600"},
        {InternalKey("aaa_alpha", "col", 500, kTypeValue), "va_500"},
        {InternalKey("aaa_alpha", "col", 400, kTypeValue), "va_400"},
        {InternalKey("aaa_alpha", "col", 300, kTypeValue), "va_300"},
        {InternalKey("aaa_alpha", "col", 200, kTypeValue), "va_200"},
        {InternalKey("aaa_alpha", "col", 100, kTypeValue), "va_100"},
        // "zzz" group — shared prefix with "aaa" is zero (different first char)
        // This is a new restart point and shared=0 with the previous key.
        {InternalKey("zzz_alpha", "col", 800, kTypeValue), "vz_800"},
        {InternalKey("zzz_alpha", "col", 700, kTypeValue), "vz_700"},
        {InternalKey("zzz_alpha", "col", 600, kTypeValue), "vz_600"},
        {InternalKey("zzz_alpha", "col", 500, kTypeValue), "vz_500"},
        {InternalKey("zzz_alpha", "col", 400, kTypeValue), "vz_400"},
        {InternalKey("zzz_alpha", "col", 300, kTypeValue), "vz_300"},
        {InternalKey("zzz_alpha", "col", 200, kTypeValue), "vz_200"},
        {InternalKey("zzz_alpha", "col", 100, kTypeValue), "vz_100"},
    };

    auto path = WriteSSTable(entries);
    SSTableReader r(path);

    // Get() for newest version of each (row, col).
    std::string out;
    EXPECT_EQ(r.Get("aaa_alpha", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "va_800");  // newest ts=800

    EXPECT_EQ(r.Get("zzz_alpha", "col", out), GetResult::kFound);
    EXPECT_EQ(out, "vz_800");  // newest ts=800

    // Full scan: all 16 entries must decode correctly in order.
    auto it = r.NewIterator();
    it.SeekToFirst();
    for (auto& [k, v] : entries) {
        EXPECT_TRUE(it.Valid());
        if (!it.Valid()) break;
        EXPECT_EQ(it.key().row,       k.row);
        EXPECT_EQ(it.key().col,       k.col);
        EXPECT_EQ(it.key().timestamp, k.timestamp);
        EXPECT_EQ(it.value(), v);
        it.Next();
    }
    EXPECT_FALSE(it.Valid());
}

TEST(DeltaCompression, deeply_shared_prefix_with_one_byte_divergence) {
    // Keys share all but the last byte of a long prefix.
    // This maximises shared/unshared ratio and stresses resize behaviour:
    // resize truncates 1 byte, append adds 1 byte, in rapid succession.

    const std::string long_prefix(60, 'x');  // 60 identical bytes
    std::vector<std::pair<InternalKey, std::string>> entries;

    for (int i = 0; i < 24; ++i) {
        // Row: 60 x's + one varying byte ('a'+i)
        std::string row = long_prefix + static_cast<char>('a' + i);
        entries.push_back({
            InternalKey(row, "col", static_cast<int64_t>(100 - i), kTypeValue),
            "v" + std::to_string(i)
        });
    }

    auto path = WriteSSTable(entries);
    SSTableReader r(path);

    // Every entry must be retrievable and correct.
    for (int i = 0; i < 24; ++i) {
        std::string row = long_prefix + static_cast<char>('a' + i);
        std::string out;
        EXPECT_EQ(r.Get(row, "col", out), GetResult::kFound);
        EXPECT_EQ(out, "v" + std::to_string(i));
    }
}

TEST(DeltaCompression, alternating_long_and_short_keys_no_bleed) {
    // Alternates between a very long key and a very short key.
    // After decoding the long key, resize(small_shared) must truncate cleanly;
    // stale bytes from the long key must not appear in the short key.
    // After the short key, resize(0) at the next restart restarts cleanly.

    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int i = 0; i < 32; ++i) {
        std::string row;
        if (i % 2 == 0) {
            // Long key: shared long prefix + index
            row = std::string(50, 'L') + std::to_string(i);
        } else {
            // Short key: just a few chars, guaranteed > previous long key lexicographically
            // We need strict ascending order, so use 'Z' prefix which beats 'L'
            row = "Z" + std::to_string(i);
        }
        entries.push_back({
            InternalKey(row, "c", static_cast<int64_t>(1000 - i), kTypeValue),
            "val" + std::to_string(i)
        });
    }

    // Sort to guarantee ascending InternalKey order
    InternalKeyComparator cmp;
    std::sort(entries.begin(), entries.end(),
              [&](auto& a, auto& b){ return cmp(a.first, b.first) < 0; });

    auto path = WriteSSTable(entries);
    SSTableReader r(path);

    // Full scan: every decoded key must match exactly what was written.
    auto it = r.NewIterator();
    it.SeekToFirst();

    // Rebuild sorted ground truth for comparison
    std::vector<std::pair<std::string,std::string>> truth;
    for (auto& [k, v] : entries) truth.push_back({k.row, v});

    for (auto& [row, val] : truth) {
        EXPECT_TRUE(it.Valid());
        if (!it.Valid()) break;
        EXPECT_EQ(it.key().row, row);
        EXPECT_EQ(it.value(), val);
        it.Next();
    }
    EXPECT_FALSE(it.Valid());
}

TEST(IteratorBoundary, next_decodes_correctly_across_UNALIGNED_block_boundary) {
    const int kTotalEntries = 50;
    std::vector<std::pair<InternalKey, std::string>> entries;
    for (int i = 0; i < kTotalEntries; ++i) {
        char row[7]; std::snprintf(row, sizeof(row), "r%05d", i);
        size_t variable_len = (i % 2 == 0) ? 300 : 700;
        entries.push_back({
            InternalKey(row, "col:f1", static_cast<int64_t>(1000 - i), kTypeValue),
            std::string(variable_len, static_cast<char>('a' + (i % 26)))
        });
    }
    auto path = WriteSSTable(entries);
    SSTableReader r(path);
    EXPECT_GT(r.NumBlocks(), 1u);
    auto it = r.NewIterator();
    it.SeekToFirst();
    for (int i = 0; i < kTotalEntries; ++i) {
        EXPECT_TRUE(it.Valid());
        if (!it.Valid()) break;
        char expected_row[7]; std::snprintf(expected_row, sizeof(expected_row), "r%05d", i);
        EXPECT_EQ(it.key().row, std::string(expected_row));
        it.Next();
    }
    EXPECT_FALSE(it.Valid());
}
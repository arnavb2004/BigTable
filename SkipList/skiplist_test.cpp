#include "skiplist.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <set>
#include <map>
#include <random>
#include <sstream>
#include <climits>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>

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

#define EXPECT_EQ(a,b)    EXPECT_TRUE((a) == (b))
#define EXPECT_NE(a,b)    EXPECT_TRUE((a) != (b))
#define EXPECT_LT(a,b)    EXPECT_TRUE((a) <  (b))
#define EXPECT_FALSE(e)   EXPECT_TRUE(!(e))
#define EXPECT_NULL(p)    EXPECT_TRUE((p) == nullptr)
#define EXPECT_NOTNULL(p) EXPECT_TRUE((p) != nullptr)

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
// Comparators and type aliases
// ─────────────────────────────────────────────────────────────────────────────

struct IntCmp {
    int operator()(int a, int b) const { return (a > b) - (a < b); }
};

struct StrCmp {
    int operator()(const std::string& a, const std::string& b) const {
        return a.compare(b);
    }
};

// Composite key: mimics a real Bigtable memtable key (row, column, timestamp).
// Timestamps are stored in DESCENDING order so newest is returned first.
struct MemKey {
    std::string row;
    std::string col;
    int64_t     ts;   // higher = newer

    bool operator==(const MemKey& o) const {
        return row == o.row && col == o.col && ts == o.ts;
    }
};

struct MemKeyCmp {
    // Sort by (row ASC, col ASC, ts DESC)
    int operator()(const MemKey& a, const MemKey& b) const {
        int rc = a.row.compare(b.row); if (rc != 0) return rc;
        int cc = a.col.compare(b.col); if (cc != 0) return cc;
        if (a.ts > b.ts) return -1;
        if (a.ts < b.ts) return  1;
        return 0;
    }
};

using IntList = SkipList<int, std::string, IntCmp>;
using StrList = SkipList<std::string, int, StrCmp>;
using MemList = SkipList<MemKey, std::string, MemKeyCmp>;

// ─────────────────────────────────────────────────────────────────────────────
// ── Suite 1: Basic insert & search
// ─────────────────────────────────────────────────────────────────────────────

TEST(Basic, EmptySearchReturnsFalse) {
    Arena a; IntList sl(IntCmp{}, &a);
    std::string out;
    EXPECT_FALSE(sl.Search(0,   out));
    EXPECT_FALSE(sl.Search(42,  out));
    EXPECT_FALSE(sl.Search(-1,  out));
}

TEST(Basic, InsertThenFind) {
    Arena a; IntList sl(IntCmp{}, &a);
    sl.Insert(10, "ten");
    std::string out;
    EXPECT_TRUE(sl.Search(10, out));
    EXPECT_EQ(out, "ten");
}

TEST(Basic, MissingKeyNotFound) {
    Arena a; IntList sl(IntCmp{}, &a);
    sl.Insert(5, "five");
    std::string out;
    EXPECT_FALSE(sl.Search(4, out));
    EXPECT_FALSE(sl.Search(6, out));
}

TEST(Basic, UpsertOverwritesValue) {
    Arena a; IntList sl(IntCmp{}, &a);
    sl.Insert(7, "seven");
    sl.Insert(7, "SEVEN");
    std::string out;
    EXPECT_TRUE(sl.Search(7, out));
    EXPECT_EQ(out, "SEVEN");
}

TEST(Basic, UpsertManyTimesKeepsLatest) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int i = 0; i < 50; ++i)
        sl.Insert(1, "v" + std::to_string(i));
    std::string out;
    EXPECT_TRUE(sl.Search(1, out));
    EXPECT_EQ(out, "v49");
}

TEST(Basic, MultipleDistinctKeysAllFound) {
    Arena a; IntList sl(IntCmp{}, &a);
    std::vector<int> keys = {9, 3, 7, 1, 5};
    for (int k : keys) sl.Insert(k, std::to_string(k));
    for (int k : keys) {
        std::string out;
        EXPECT_TRUE(sl.Search(k, out));
        EXPECT_EQ(out, std::to_string(k));
    }
}

TEST(Basic, NegativeZeroPositiveKeys) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int k : {-100, -1, 0, 1, 100})
        sl.Insert(k, std::to_string(k));
    for (int k : {-100, -1, 0, 1, 100}) {
        std::string out;
        EXPECT_TRUE(sl.Search(k, out));
        EXPECT_EQ(out, std::to_string(k));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ── Suite 2: Iterator — basic
// ─────────────────────────────────────────────────────────────────────────────

TEST(Iterator, EmptyListIsInvalid) {
    Arena a; IntList sl(IntCmp{}, &a);
    IntList::Iterator it(&sl);
    it.SeekToFirst();
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, SingleElementTraversal) {
    Arena a; IntList sl(IntCmp{}, &a);
    sl.Insert(42, "x");
    IntList::Iterator it(&sl);
    it.SeekToFirst();
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key(), 42);
    EXPECT_EQ(it.Value(), "x");
    it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, ForwardOrderMatchesSorted) {
    Arena a; IntList sl(IntCmp{}, &a);
    std::vector<int> keys = {5, 3, 8, 1, 9, 2, 7, 4, 6};
    for (int k : keys) sl.Insert(k, std::to_string(k));

    std::vector<int> sorted = keys;
    std::sort(sorted.begin(), sorted.end());

    IntList::Iterator it(&sl);
    it.SeekToFirst();
    for (int expected : sorted) {
        EXPECT_TRUE(it.Valid());
        EXPECT_EQ(it.Key(), expected);
        EXPECT_EQ(it.Value(), std::to_string(expected));
        it.Next();
    }
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, SeekExactKeyLands) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int i = 1; i <= 10; ++i) sl.Insert(i * 10, std::to_string(i * 10));

    IntList::Iterator it(&sl);
    it.Seek(50);
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key(), 50);
}

TEST(Iterator, SeekBetweenKeysLandsOnNext) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int k : {10, 20, 30, 40, 50}) sl.Insert(k, "x");

    IntList::Iterator it(&sl);
    it.Seek(25);
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key(), 30);
}

TEST(Iterator, SeekBeforeMinLandsOnFirst) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int k : {10, 20, 30}) sl.Insert(k, "x");

    IntList::Iterator it(&sl);
    it.Seek(-999);
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key(), 10);
}

TEST(Iterator, SeekPastMaxIsInvalid) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int k : {1, 2, 3}) sl.Insert(k, "x");

    IntList::Iterator it(&sl);
    it.Seek(100);
    EXPECT_FALSE(it.Valid());
}

TEST(Iterator, SeekToFirstRewindsAfterSeek) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int k : {1, 3, 5, 7}) sl.Insert(k, "x");

    IntList::Iterator it(&sl);
    it.Seek(7);
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key(), 7);

    it.SeekToFirst();
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key(), 1);
}

TEST(Iterator, IteratorValueMatchesSearch) {
    Arena a; IntList sl(IntCmp{}, &a);
    std::map<int,std::string> ref;
    for (int k : {4, 8, 2, 6, 1}) {
        std::string v = "val" + std::to_string(k);
        sl.Insert(k, v);
        ref[k] = v;
    }
    IntList::Iterator it(&sl);
    it.SeekToFirst();
    for (auto& [k, v] : ref) { // map is sorted
        EXPECT_TRUE(it.Valid());
        EXPECT_EQ(it.Key(),   k);
        EXPECT_EQ(it.Value(), v);
        it.Next();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ── Suite 3: Ordering invariants
// ─────────────────────────────────────────────────────────────────────────────

TEST(Order, NoDuplicatesAfterUpsert) {
    // Insert same key many times; iterator must visit it exactly once.
    Arena a; IntList sl(IntCmp{}, &a);
    for (int i = 0; i < 30; ++i) sl.Insert(5, "v");
    for (int k : {1, 3, 7, 9}) sl.Insert(k, std::to_string(k));

    IntList::Iterator it(&sl);
    it.SeekToFirst();
    std::vector<int> seen;
    while (it.Valid()) { seen.push_back(it.Key()); it.Next(); }
    EXPECT_EQ(seen, (std::vector<int>{1, 3, 5, 7, 9}));
}

TEST(Order, StrictlyAscendingTraversal) {
    Arena a; IntList sl(IntCmp{}, &a);
    std::mt19937 rng(99);
    std::uniform_int_distribution<int> dist(-500, 500);
    for (int i = 0; i < 200; ++i) sl.Insert(dist(rng), "v");

    IntList::Iterator it(&sl);
    it.SeekToFirst();
    int prev = INT_MIN;
    while (it.Valid()) {
        EXPECT_LT(prev, it.Key()); // strictly less (no duplicates in list)
        prev = it.Key();
        it.Next();
    }
}

TEST(Order, SeekThenNextRemainsAscending) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int i = 0; i < 50; ++i) sl.Insert(i * 3, std::to_string(i));

    IntList::Iterator it(&sl);
    it.Seek(30);
    EXPECT_TRUE(it.Valid());
    int prev = it.Key();
    it.Next();
    while (it.Valid()) {
        EXPECT_LT(prev, it.Key());
        prev = it.Key();
        it.Next();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ── Suite 4: String keys
// ─────────────────────────────────────────────────────────────────────────────

TEST(StringKey, InsertAndFind) {
    Arena a; StrList sl(StrCmp{}, &a);
    sl.Insert("banana", 2);
    sl.Insert("apple",  1);
    sl.Insert("cherry", 3);
    int out;
    EXPECT_TRUE(sl.Search("apple",  out)); EXPECT_EQ(out, 1);
    EXPECT_TRUE(sl.Search("banana", out)); EXPECT_EQ(out, 2);
    EXPECT_TRUE(sl.Search("cherry", out)); EXPECT_EQ(out, 3);
    EXPECT_FALSE(sl.Search("grape", out));
}

TEST(StringKey, LexicographicOrder) {
    Arena a; StrList sl(StrCmp{}, &a);
    for (auto& s : {"zebra","ant","mango","fig","kiwi"})
        sl.Insert(s, 0);

    StrList::Iterator it(&sl);
    it.SeekToFirst();
    std::vector<std::string> seen;
    while (it.Valid()) { seen.push_back(it.Key()); it.Next(); }
    std::vector<std::string> expected = {"ant","fig","kiwi","mango","zebra"};
    EXPECT_EQ(seen, expected);
}

TEST(StringKey, SeekPrefix) {
    Arena a; StrList sl(StrCmp{}, &a);
    for (auto& s : {"com.google.maps","com.cnn.www","com.amazon.shop","org.apache"})
        sl.Insert(s, 0);

    StrList::Iterator it(&sl);
    it.Seek("com.cnn");  // should land on com.cnn.www
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key(), "com.cnn.www");
}

// ─────────────────────────────────────────────────────────────────────────────
// ── Suite 5: Composite MemKey (Bigtable-style)
// ─────────────────────────────────────────────────────────────────────────────

TEST(MemKey, NewestTimestampFirstForSameRowCol) {
    Arena a; MemList sl(MemKeyCmp{}, &a);
    sl.Insert({"row1","col:a", 100}, "v_100");
    sl.Insert({"row1","col:a", 300}, "v_300");
    sl.Insert({"row1","col:a", 200}, "v_200");

    // SeekToFirst on row1/col:a should give ts=300 (descending ts)
    MemList::Iterator it(&sl);
    it.Seek({"row1","col:a", INT64_MAX});
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key().ts, 300);
    it.Next();
    EXPECT_EQ(it.Key().ts, 200);
    it.Next();
    EXPECT_EQ(it.Key().ts, 100);
}

TEST(MemKey, DifferentRowsAreSegregated) {
    Arena a; MemList sl(MemKeyCmp{}, &a);
    sl.Insert({"rowB","col:x", 1}, "B");
    sl.Insert({"rowA","col:x", 1}, "A");
    sl.Insert({"rowC","col:x", 1}, "C");

    MemList::Iterator it(&sl);
    it.SeekToFirst();
    EXPECT_EQ(it.Key().row, "rowA"); it.Next();
    EXPECT_EQ(it.Key().row, "rowB"); it.Next();
    EXPECT_EQ(it.Key().row, "rowC"); it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(MemKey, SeekToExactRow) {
    Arena a; MemList sl(MemKeyCmp{}, &a);
    sl.Insert({"rowA","col:x",1}, "A");
    sl.Insert({"rowB","col:x",1}, "B");
    sl.Insert({"rowC","col:x",1}, "C");

    MemList::Iterator it(&sl);
    it.Seek({"rowB", "", INT64_MAX}); // seek to start of rowB
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key().row, "rowB");
}

TEST(MemKey, SearchExactKey) {
    Arena a; MemList sl(MemKeyCmp{}, &a);
    sl.Insert({"r1","c:a",10}, "hello");
    std::string out;
    EXPECT_TRUE(sl.Search({"r1","c:a",10}, out));
    EXPECT_EQ(out, "hello");
    EXPECT_FALSE(sl.Search({"r1","c:a",99}, out));
}

// ─────────────────────────────────────────────────────────────────────────────
// ── Suite 6: Large scale
// ─────────────────────────────────────────────────────────────────────────────

TEST(Scale, ThousandRandomInsertsSorted) {
    Arena a; IntList sl(IntCmp{}, &a);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 9999);

    std::set<int> expected;
    for (int i = 0; i < 1000; ++i) {
        int k = dist(rng);
        expected.insert(k);
        sl.Insert(k, std::to_string(k));
    }

    IntList::Iterator it(&sl);
    it.SeekToFirst();
    for (int k : expected) {
        EXPECT_TRUE(it.Valid());
        if (it.Valid()) { EXPECT_EQ(it.Key(), k); it.Next(); }
    }
    EXPECT_FALSE(it.Valid());
}

TEST(Scale, AllInsertedKeysSearchable) {
    Arena a; IntList sl(IntCmp{}, &a);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> dist(0, 4999);

    std::set<int> inserted;
    for (int i = 0; i < 500; ++i) {
        int k = dist(rng);
        inserted.insert(k);
        sl.Insert(k, std::to_string(k));
    }
    for (int k : inserted) {
        std::string out;
        EXPECT_TRUE(sl.Search(k, out));
        EXPECT_EQ(out, std::to_string(k));
    }
}

TEST(Scale, RandomUpsertConsistentWithMap) {
    // Ground truth: std::map mirrors every Insert/upsert.
    // Then verify the skiplist matches it exactly via iterator.
    Arena a; IntList sl(IntCmp{}, &a);
    std::map<int,std::string> truth;
    std::mt19937 rng(13);
    std::uniform_int_distribution<int> kdist(0, 99); // small key space -> lots of upserts

    for (int i = 0; i < 500; ++i) {
        int k = kdist(rng);
        std::string v = "v" + std::to_string(i);
        sl.Insert(k, v);
        truth[k] = v;
    }

    IntList::Iterator it(&sl);
    it.SeekToFirst();
    for (auto& [k, v] : truth) {
        EXPECT_TRUE(it.Valid());
        if (it.Valid()) {
            EXPECT_EQ(it.Key(),   k);
            EXPECT_EQ(it.Value(), v);
            it.Next();
        }
    }
    EXPECT_FALSE(it.Valid());
}

TEST(Scale, FiveThousandInsertsThenSeek) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int i = 0; i < 5000; ++i) sl.Insert(i * 2, std::to_string(i)); // evens only

    // Seek to each odd number; should always land on the next even.
    for (int target = 1; target < 9999; target += 2) {
        IntList::Iterator it(&sl);
        it.Seek(target);
        EXPECT_TRUE(it.Valid());
        if (it.Valid()) EXPECT_EQ(it.Key(), target + 1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ── Suite 7: Arena integration
// ─────────────────────────────────────────────────────────────────────────────

TEST(Arena, MemoryUsageGrowsWithInserts) {
    Arena a;
    size_t before = a.MemoryUsage();
    IntList sl(IntCmp{}, &a);
    for (int i = 0; i < 200; ++i) sl.Insert(i, std::to_string(i));
    EXPECT_TRUE(a.MemoryUsage() > before);
}

TEST(Arena, NodesAllocatedFromArena) {
    // Two skip lists sharing one Arena: both must work correctly.
    Arena a;
    IntList sl1(IntCmp{}, &a);
    IntList sl2(IntCmp{}, &a);
    for (int i = 0; i < 50; ++i) sl1.Insert(i,    "a");
    for (int i = 0; i < 50; ++i) sl2.Insert(i+50, "b");

    std::string out;
    EXPECT_TRUE(sl1.Search(25,  out)); EXPECT_EQ(out, "a");
    EXPECT_TRUE(sl2.Search(75,  out)); EXPECT_EQ(out, "b");
    EXPECT_FALSE(sl1.Search(75, out));
    EXPECT_FALSE(sl2.Search(25, out));
}


// ─────────────────────────────────────────────────────────────────────────────
// ── Suite 8: Boundary & Overflow
// ─────────────────────────────────────────────────────────────────────────────

TEST(Boundary, INT_MIN_and_INT_MAX_Keys) {
    // The comparator uses subtraction-free (a>b)-(a<b) so no overflow.
    Arena a; IntList sl(IntCmp{}, &a);
    sl.Insert(INT_MIN, "min");
    sl.Insert(INT_MAX, "max");
    sl.Insert(0,       "zero");

    std::string out;
    EXPECT_TRUE(sl.Search(INT_MIN, out)); EXPECT_EQ(out, "min");
    EXPECT_TRUE(sl.Search(INT_MAX, out)); EXPECT_EQ(out, "max");
    EXPECT_TRUE(sl.Search(0,       out)); EXPECT_EQ(out, "zero");

    // Iterator must visit in correct order: INT_MIN, 0, INT_MAX
    IntList::Iterator it(&sl);
    it.SeekToFirst();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), INT_MIN); it.Next();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), 0);       it.Next();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), INT_MAX); it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(Boundary, SeekToINT_MIN) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int k : {-10, 0, 10}) sl.Insert(k, std::to_string(k));

    IntList::Iterator it(&sl);
    it.Seek(INT_MIN);
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key(), -10);  // first key >= INT_MIN
}

TEST(Boundary, SeekToINT_MAX) {
    Arena a; IntList sl(IntCmp{}, &a);
    for (int k : {-10, 0, 10}) sl.Insert(k, std::to_string(k));

    IntList::Iterator it(&sl);
    it.Seek(INT_MAX);
    EXPECT_FALSE(it.Valid());  // nothing >= INT_MAX in the list
}

TEST(Boundary, INT64_Timestamps) {
    // Bigtable timestamps are int64. Verify INT64_MIN/MAX don't corrupt order.
    Arena a; MemList sl(MemKeyCmp{}, &a);
    sl.Insert({"row","col", INT64_MAX}, "newest");
    sl.Insert({"row","col", 0},         "zero");
    sl.Insert({"row","col", INT64_MIN}, "oldest");

    MemList::Iterator it(&sl);
    it.Seek({"row","col", INT64_MAX});
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key().ts, INT64_MAX); it.Next();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key().ts, 0);         it.Next();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key().ts, INT64_MIN); it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(Boundary, EmptyStringKey) {
    Arena a; StrList sl(StrCmp{}, &a);
    sl.Insert("",    0);
    sl.Insert("a",   1);
    sl.Insert("aa",  2);

    // Empty string is lexicographically smallest
    StrList::Iterator it(&sl);
    it.SeekToFirst();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), "");   it.Next();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), "a");  it.Next();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), "aa"); it.Next();
    EXPECT_FALSE(it.Valid());

    int out;
    EXPECT_TRUE(sl.Search("", out)); EXPECT_EQ(out, 0);
}

TEST(Boundary, SeekEmptyString) {
    Arena a; StrList sl(StrCmp{}, &a);
    sl.Insert("apple",  1);
    sl.Insert("banana", 2);

    // Seek("") should land on the first key since "" < everything
    StrList::Iterator it(&sl);
    it.Seek("");
    EXPECT_TRUE(it.Valid());
    EXPECT_EQ(it.Key(), "apple");
}

TEST(Boundary, VeryLongStringKey) {
    Arena a; StrList sl(StrCmp{}, &a);
    std::string longkey(10000, 'x'); // 10KB key
    std::string longkey2(10000, 'y');
    sl.Insert(longkey,  1);
    sl.Insert(longkey2, 2);
    sl.Insert("short",  3);

    int out;
    EXPECT_TRUE(sl.Search(longkey,  out)); EXPECT_EQ(out, 1);
    EXPECT_TRUE(sl.Search(longkey2, out)); EXPECT_EQ(out, 2);
    EXPECT_TRUE(sl.Search("short",  out)); EXPECT_EQ(out, 3);

    // Order: "short" < 10000 x's < 10000 y's
    StrList::Iterator it(&sl);
    it.SeekToFirst();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), "short");   it.Next();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), longkey);   it.Next();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), longkey2);  it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(Boundary, SingleKeyListBoundaries) {
    Arena a; IntList sl(IntCmp{}, &a);
    sl.Insert(42, "only");

    IntList::Iterator it(&sl);

    it.Seek(41);  // just before: should land on 42
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), 42);

    it.Seek(42);  // exact
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), 42);

    it.Seek(43);  // just after: nothing
    EXPECT_FALSE(it.Valid());
}

TEST(Boundary, MaxHeightStressWithSingleKey) {
    // Repeatedly insert one key to stress the height-promotion path.
    // The list must remain consistent no matter how high a node grows.
    Arena a; IntList sl(IntCmp{}, &a);
    // Insert the same key 10000 times — exercises upsert on every height.
    for (int i = 0; i < 10000; ++i)
        sl.Insert(7, "v" + std::to_string(i));

    std::string out;
    EXPECT_TRUE(sl.Search(7, out));
    EXPECT_EQ(out, "v9999");

    IntList::Iterator it(&sl);
    it.SeekToFirst();
    EXPECT_TRUE(it.Valid()); EXPECT_EQ(it.Key(), 7); it.Next();
    EXPECT_FALSE(it.Valid());
}

TEST(Boundary, ConsecutiveIntegerRange) {
    // All integers [0, 999]: no gaps, seek to every key must be exact.
    Arena a; IntList sl(IntCmp{}, &a);
    for (int i = 0; i < 1000; ++i) sl.Insert(i, std::to_string(i));

    for (int i = 0; i < 1000; ++i) {
        IntList::Iterator it(&sl);
        it.Seek(i);
        EXPECT_TRUE(it.Valid());
        EXPECT_EQ(it.Key(), i);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ── Suite 9: Concurrency
//
// Thread-safety model: concurrent READS are safe; writes are serialised by
// the caller's mutex.  These tests verify:
//   (a) Many concurrent readers see consistent data after a write phase.
//   (b) Serialised writers + concurrent readers produce no corruption.
//   (c) The iterator is safe to use from multiple reader threads simultaneously.
// ─────────────────────────────────────────────────────────────────────────────

TEST(Concurrency, ConcurrentReadsAfterWrites) {
    // Phase 1 (single thread): insert 2000 keys.
    // Phase 2 (many threads):  each reader scans the whole list and checks
    //                          that every inserted key is present and correct.
    Arena a; IntList sl(IntCmp{}, &a);
    const int N = 2000;
    for (int i = 0; i < N; ++i) sl.Insert(i, std::to_string(i));

    const int READERS = 8;
    std::atomic<int> failures{0};

    auto read_task = [&]() {
        for (int i = 0; i < N; ++i) {
            std::string out;
            if (!sl.Search(i, out))           { ++failures; continue; }
            if (out != std::to_string(i))     { ++failures; }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < READERS; ++t)
        threads.emplace_back(read_task);
    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0);
}

TEST(Concurrency, ConcurrentIteratorsAfterWrites) {
    // Multiple threads each create their own Iterator and do a full scan.
    // They must all observe the same sorted sequence.
    Arena a; IntList sl(IntCmp{}, &a);
    const int N = 500;
    for (int i = 0; i < N; ++i) sl.Insert(i * 2, std::to_string(i * 2));

    std::atomic<int> failures{0};

    auto scan_task = [&]() {
        IntList::Iterator it(&sl);
        it.SeekToFirst();
        int prev = -1;
        int count = 0;
        while (it.Valid()) {
            if (it.Key() <= prev) { ++failures; }  // must be strictly ascending
            prev = it.Key();
            ++count;
            it.Next();
        }
        if (count != N) { ++failures; }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back(scan_task);
    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0);
}

TEST(Concurrency, ConcurrentSeeksAfterWrites) {
    // Each reader thread does random Seeks and verifies the result is
    // the correct lower-bound (first key >= target).
    Arena a; IntList sl(IntCmp{}, &a);
    // Insert only even numbers: 0, 2, 4, …, 998
    for (int i = 0; i < 500; ++i) sl.Insert(i * 2, std::to_string(i * 2));

    std::atomic<int> failures{0};

    auto seek_task = [&](int seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> dist(0, 999);
        for (int i = 0; i < 200; ++i) {
            int target = dist(rng);
            IntList::Iterator it(&sl);
            it.Seek(target);
            if (target >= 998) {
                // Only 998 exists at the top; 999 should be past end
                if (target == 998) {
                    if (!it.Valid() || it.Key() != 998) ++failures;
                } else {
                    if (it.Valid()) ++failures;  // 999 should be past end
                }
            } else {
                // Expected landing: next even >= target
                int expected = (target % 2 == 0) ? target : target + 1;
                if (!it.Valid() || it.Key() != expected) ++failures;
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back(seek_task, t * 17);
    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0);
}

TEST(Concurrency, SerializedWritesConcurrentReads) {
    // Writer thread inserts keys one at a time (holding a mutex for each write).
    // Reader threads continuously search for already-published keys.
    // A reader may only look up a key after it has been announced via the
    // atomic counter `published`, so it never races with an in-progress write.
    Arena a; IntList sl(IntCmp{}, &a);
    std::mutex write_mutex;
    std::atomic<int> published{0};   // readers only search keys < published
    std::atomic<bool> done{false};
    std::atomic<int> failures{0};

    const int N = 1000;

    auto writer = [&]() {
        for (int i = 0; i < N; ++i) {
            { std::lock_guard<std::mutex> lk(write_mutex);
              sl.Insert(i, std::to_string(i)); }
            published.fetch_add(1, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
    };

    auto reader = [&]() {
        while (!done.load(std::memory_order_acquire)) {
            int limit = published.load(std::memory_order_acquire);
            for (int i = 0; i < limit; ++i) {
                std::string out;
                if (!sl.Search(i, out))           { ++failures; continue; }
                if (out != std::to_string(i))     { ++failures; }
            }
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back(writer);
    for (int t = 0; t < 4; ++t)
        threads.emplace_back(reader);
    for (auto& t : threads) t.join();

    // Final check: all N keys must be present.
    for (int i = 0; i < N; ++i) {
        std::string out;
        if (!sl.Search(i, out) || out != std::to_string(i)) ++failures;
    }

    EXPECT_EQ(failures.load(), 0);
}

TEST(Concurrency, MultipleArenasSeparateThreads) {
    // Each thread owns its own Arena + SkipList.
    // No sharing — validates there is no accidental global state.
    std::atomic<int> failures{0};

    auto thread_task = [&](int base) {
        Arena a;
        IntList sl(IntCmp{}, &a);
        for (int i = 0; i < 100; ++i) sl.Insert(base + i, std::to_string(base + i));
        for (int i = 0; i < 100; ++i) {
            std::string out;
            if (!sl.Search(base + i, out))               { ++failures; continue; }
            if (out != std::to_string(base + i))          { ++failures; }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back(thread_task, t * 1000);
    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "\n=== SkipList Unit Tests ===\n\n";
    if (g_failed == 0) {
        std::cout << "Results : " << g_passed << " / " << g_total << " assertions passed\n";
        std::cout << "ALL TESTS PASSED \n\n";
        return 0;
    }
    std::cout << "Results : " << g_passed << " / " << g_total << " assertions passed\n";
    std::cout << g_failed << " ASSERTION(S) FAILED ✗\n\n";
    return 1;
}
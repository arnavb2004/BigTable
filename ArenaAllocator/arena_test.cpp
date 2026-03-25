// arena_test.cpp
// Compile: g++ -std=c++17 -Wall -Wextra -g arena_test.cpp arena.cpp -o arena_test
// Run:     ./arena_test

#include "arena.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Tiny test framework (matches skiplist_test style)
// ─────────────────────────────────────────────────────────────────────────────
static int g_total  = 0;
static int g_passed = 0;

#define EXPECT_TRUE(expr) do { \
    ++g_total; \
    if (!(expr)) { \
        std::cerr << "  FAIL: " << #expr << "  (" __FILE__ ":" << __LINE__ << ")\n"; \
    } else { ++g_passed; } \
} while(0)

#define EXPECT_EQ(a, b) EXPECT_TRUE((a) == (b))
#define EXPECT_NE(a, b) EXPECT_TRUE((a) != (b))
#define EXPECT_GE(a, b) EXPECT_TRUE((a) >= (b))
#define EXPECT_NULL(p)  EXPECT_TRUE((p) == nullptr)
#define EXPECT_NOTNULL(p) EXPECT_TRUE((p) != nullptr)

#define TEST(name) \
    static void name(); \
    struct name##_register { name##_register() { \
        std::cout << "[TEST] " #name "\n"; \
        name(); \
    }} name##_instance; \
    static void name()

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(ZeroByteAllocateReturnsNull) {
    Arena arena;
    EXPECT_NULL(arena.Allocate(0));
}

TEST(ZeroByteAllocateAlignedReturnsNull) {
    Arena arena;
    EXPECT_NULL(arena.AllocateAligned(0));
}

TEST(BasicAllocateReturnsNonNull) {
    Arena arena;
    char* p = arena.Allocate(100);
    EXPECT_NOTNULL(p);
    EXPECT_GE(arena.MemoryUsage(), static_cast<size_t>(100));
}

TEST(AllocateIsWritable) {
    Arena arena;
    const size_t N = 256;
    char* p = arena.Allocate(N);
    EXPECT_NOTNULL(p);
    memset(p, 0xAB, N);
    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(static_cast<unsigned char>(p[i]), 0xABu);
    }
}

TEST(AllocateAlignedIsWritable) {
    Arena arena;
    const size_t N = 128;
    char* p = arena.AllocateAligned(N);
    EXPECT_NOTNULL(p);
    memset(p, 0xCD, N);
    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(static_cast<unsigned char>(p[i]), 0xCDu);
    }
}

TEST(AllocateAlignedAddressIsAligned) {
    Arena arena;
    std::vector<size_t> sizes = {1, 3, 1, 7, 2, 5, 16, 1, 9, 3};
    for (size_t sz : sizes) {
        char* p = arena.AllocateAligned(sz);
        EXPECT_NOTNULL(p);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, static_cast<uintptr_t>(0));
    }
}

TEST(AllocateAlignedAfterFallbackIsAligned) {
    Arena arena;
    arena.Allocate(4090);
    char* p = arena.AllocateAligned(9);
    EXPECT_NOTNULL(p);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, static_cast<uintptr_t>(0));
}

TEST(HugeAllocationDedicatedBlock) {
    Arena arena;
    char* p = arena.Allocate(10000);
    EXPECT_NOTNULL(p);
    EXPECT_GE(arena.MemoryUsage(), static_cast<size_t>(10000));
}

TEST(HugeAllocationDoesNotCorruptBumpPointer) {
    Arena arena;
    arena.Allocate(2000);
    char* p = arena.Allocate(10);
    EXPECT_NOTNULL(p);
    char* q = arena.Allocate(10);
    EXPECT_NOTNULL(q);
    EXPECT_NE(p, q);
}

TEST(ConsecutiveAllocationsDoNotOverlap) {
    Arena arena;
    const size_t SZ = 64;
    char* a = arena.Allocate(SZ);
    char* b = arena.Allocate(SZ);
    EXPECT_NOTNULL(a);
    EXPECT_NOTNULL(b);
    if (b > a) {
        EXPECT_TRUE(static_cast<size_t>(b - a) >= SZ);
    } else {
        EXPECT_TRUE(static_cast<size_t>(a - b) >= SZ);
    }
}

TEST(MemoryUsageMonotonicallyIncreases) {
    Arena arena;
    size_t prev = arena.MemoryUsage();
    for (int i = 0; i < 20; ++i) {
        arena.Allocate(200);
        size_t cur = arena.MemoryUsage();
        EXPECT_TRUE(cur >= prev);
        prev = cur;
    }
}

TEST(MemoryUsageAccountsForBlockBytes) {
    // First Allocate triggers AllocateNewBlock(4096).
    // memory_usage_ = 4096 + sizeof(char*) (pointer overhead in blocks_ vec).
    Arena arena;
    arena.Allocate(1);
    EXPECT_EQ(arena.MemoryUsage(), static_cast<size_t>(4096 + sizeof(char*)));
}

TEST(StressManySmallAllocations) {
    Arena arena;
    const int ITERS = 1000;
    std::vector<char*> ptrs;
    ptrs.reserve(ITERS);
    for (int i = 1; i <= ITERS; ++i) {
        size_t sz = (i % 128) + 1;
        char* p = arena.Allocate(sz);
        EXPECT_NOTNULL(p);
        ptrs.push_back(p);
    }
    for (int i = 0; i < ITERS; ++i)
        for (int j = i + 1; j < ITERS; ++j)
            EXPECT_NE(ptrs[i], ptrs[j]);
}

TEST(StressAlignedAllocations) {
    Arena arena;
    for (int i = 1; i <= 200; ++i) {
        size_t sz = (i % 64) + 1;
        char* p = arena.AllocateAligned(sz);
        EXPECT_NOTNULL(p);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, static_cast<uintptr_t>(0));
    }
}

TEST(MixedAllocateAndAllocateAligned) {
    Arena arena;
    for (int i = 0; i < 100; ++i) {
        char* p = (i % 2 == 0)
            ? arena.Allocate(i + 1)
            : arena.AllocateAligned(i + 1);
        EXPECT_NOTNULL(p);
        if (i % 2 != 0)
            EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, static_cast<uintptr_t>(0));
    }
}

TEST(MultipleArenaInstancesAreIndependent) {
    Arena a1, a2;
    char* p1 = a1.Allocate(100);
    char* p2 = a2.Allocate(100);
    EXPECT_NOTNULL(p1);
    EXPECT_NOTNULL(p2);
    EXPECT_NE(p1, p2);
    EXPECT_GE(a1.MemoryUsage(), static_cast<size_t>(100));
    EXPECT_GE(a2.MemoryUsage(), static_cast<size_t>(100));
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "\n=== Arena Unit Tests ===\n\n";
    std::cout << "Results: " << g_passed << " / " << g_total << " assertions passed.\n";
    if (g_passed == g_total) {
        std::cout << "ALL TESTS PASSED \n\n";
        return 0;
    }
    std::cout << (g_total - g_passed) << " ASSERTION(S) FAILED ✗\n\n";
    return 1;
}
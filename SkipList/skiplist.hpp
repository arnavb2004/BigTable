#ifndef SKIPLIST_HPP
#define SKIPLIST_HPP

#include "../ArenaAllocator/arena.hpp"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// SkipList<key, value, Comparator>
//
// A lock-free-read, single-writer skip list backed by an Arena allocator.
// Designed to serve as the Memtable in a Bigtable / LSM-tree implementation.
//
// Thread-safety model (mirrors LevelDB):
//   • Concurrent reads are safe without any external locking.
//   • Writes must be serialised by the caller (e.g. a per-tablet mutex).
//   • Atomic loads/stores with acquire/release ordering ensure readers never
//     observe a partially-constructed node.
//
// Template parameters:
//   key        – must be default-constructible (needed for the sentinel head).
//   value      – must be copyable and default-constructible.
//   Comparator – callable: int cmp(const key& a, const key& b)
//                returns <0, 0, >0 (same contract as strcmp).
// ─────────────────────────────────────────────────────────────────────────────

template<typename key, typename value, typename Comparator>
class SkipList {
private:
    // ── Node ─────────────────────────────────────────────────────────────────
    // MUST be declared before Iterator — Iterator holds a Node* member, so
    // Node must be a complete type before Iterator is defined.
    //
    // next_[] is a variable-length array of atomic pointers, one per level.
    // We over-allocate from the Arena so that next_[0..height-1] are valid.
    //
    // Note on `v`: std::atomic requires a trivially-copyable type, which
    // std::string is NOT.  Value writes are serialised by the caller's mutex
    // (same guarantee as LevelDB's MemTable), so we store `v` as a plain
    // member protected by the release fence on the next_[0] publication.
    struct Node {
        key   const k;
        value       v;   // plain; mutation serialised externally

        explicit Node(const key& k, const value& v) : k(k), v(v) {}

        // Level-indexed accessors with explicit memory ordering.
        Node* Next(int level) const {
            assert(level >= 0);
            return next_[level].load(std::memory_order_acquire);
        }
        void SetNext(int level, Node* x) {
            assert(level >= 0);
            next_[level].store(x, std::memory_order_release);
        }
        // Relaxed variants used during single-threaded list construction.
        Node* NextRelaxed(int level) const {
            return next_[level].load(std::memory_order_relaxed);
        }
        void SetNextRelaxed(int level, Node* x) {
            next_[level].store(x, std::memory_order_relaxed);
        }

    private:
        // Flexible array: allocated with (height-1) extra slots by NewNode.
        std::atomic<Node*> next_[1];
    };

public:
    // ── Public interface ──────────────────────────────────────────────────────

    // Construct with a comparator and a pre-existing Arena.
    // The SkipList does NOT own the Arena; the caller manages its lifetime.
    explicit SkipList(Comparator cmp, Arena* arena);

    // Insert or update a key-value pair.
    // If the key already exists its value is updated in-place.
    void Insert(const key& k, const value& v);

    // Search for a key.  If found, writes the value into `v` and returns true.
    bool Search(const key& k, value& v) const;

    // ── Iterator ──────────────────────────────────────────────────────────────
    // Forward-only iterator.  Always start with SeekToFirst() or Seek().
    // Node is already a complete type here (declared above), so Node* is valid.
    class Iterator {
    public:
        explicit Iterator(const SkipList* list);

        bool         Valid()   const;
        const key&   Key()     const;
        const value& Value()   const;

        void Next();
        void SeekToFirst();
        void Seek(const key& target);  // position at first node >= target

    private:
        const SkipList* list_;
        Node*           node_;  // Node is complete — declared before Iterator
    };

private:
    // ── Constants ────────────────────────────────────────────────────────────
    static constexpr int   kMaxHeight  = 12;
    static constexpr float kBranchProb = 0.25f;  // P(promote to next level)

    // ── Members ──────────────────────────────────────────────────────────────
    Comparator      const compare_;
    Arena*          const arena_;
    Node*           const head_;        // sentinel; key is never compared
    std::atomic<int>      max_height_;  // highest level currently in use

    // ── Private helpers ──────────────────────────────────────────────────────

    // Allocate a Node of the given height from the Arena.
    Node* NewNode(const key& k, const value& v, int height);

    // Draw a random height in [1, kMaxHeight].
    int RandomHeight();

    // Compare two keys; negative → a<b, zero → a==b, positive → a>b.
    int KeyCompare(const key& a, const key& b) const {
        return compare_(a, b);
    }

    // Return true if node n's key is strictly less than k.
    bool KeyIsLessThan(Node* n, const key& k) const {
        return n != nullptr && KeyCompare(n->k, k) < 0;
    }

    // Core traversal: returns first node with key >= k.
    // If prev != nullptr, fills prev[level] with the splice-point predecessor
    // on each level (used by Insert).
    Node* FindGreaterOrEqual(const key& k, Node** prev) const;

    // Returns the last node in the list (used for future Delete support).
    Node* FindLast() const;

    // Non-copyable.
    SkipList(const SkipList&) = delete;
    void operator=(const SkipList&) = delete;
};

#include "skiplist.ipp"
#endif // SKIPLIST_HPP
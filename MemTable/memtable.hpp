#ifndef MEMTABLE_HPP
#define MEMTABLE_HPP

#include "internal_key.hpp"
#include "../SkipList/skiplist.hpp"  // pulls in arena.hpp transitively
#include <string>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// The Memtable is the in-memory write buffer for a Bigtable tablet.
// It is the public interface layer that sits above the SkipList and Arena —
// callers never interact with either directly.
//
// Ownership:
//   Memtable owns its Arena.
//   Memtable owns its SkipList (which uses the Arena to allocate nodes).
//   Both are internal implementation details — swappable without changing
//   the public interface.
//
// Write path:
//   Put()    → inserts kTypeValue entry into SkipList
//   Delete() → inserts kTypeDeletion tombstone into SkipList
//
// Read path:
//   Get()    → seeks to (row, col, INT64_MAX, kTypeValue) and returns
//              the newest entry for that (row, col) pair
//   Iterator → walks every entry in sorted order (used by compaction)
//
// Size tracking:
//   ApproximateSize() returns Arena memory usage.
//   When this exceeds a threshold, the Tablet freezes this Memtable,
//   flushes it to an SSTable, and creates a fresh one.
//
// Thread safety:
//   Follows the same contract as SkipList —
//   writes must be serialised by the caller (e.g. a per-tablet mutex).
//   concurrent reads are safe.
// ─────────────────────────────────────────────────────────────────────────────


// ── Memtable ──────────────────────────────────────────────────────────────────
class Memtable {
public:

    // Public type alias for the underlying SkipList.
    // Exposed so that Iterator can reference Table::Iterator.
    // Do not depend on this type externally — it is an implementation detail
    // that may change when the underlying data structure is replaced.
    using Table = SkipList<InternalKey, std::string, InternalKeyComparator>;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    Memtable();
    ~Memtable() = default;

    // Non-copyable — owns Arena and SkipList
    Memtable(const Memtable&)            = delete;
    Memtable& operator=(const Memtable&) = delete;

    // ── Iterator ──────────────────────────────────────────────────────────────
    // Walks every entry in the Memtable in sorted order:
    //   row ASC → col ASC → timestamp DESC → type DESC
    //
    // Used by minor compaction to read all entries and write them
    // sequentially to an SSTable. Also used for range scans.
    //
    // Usage:
    //   Memtable::Iterator it = mem.NewIterator();
    //   for (it.SeekToFirst(); it.Valid(); it.Next()) {
    //       it.key();    // InternalKey
    //       it.value();  // std::string
    //   }
    class Iterator {
    public:
        explicit Iterator(const Memtable* mem);

        // Returns true if the iterator is positioned at a valid entry.
        bool Valid() const;

        // Returns the key at the current position. Requires Valid().
        const InternalKey& key() const;

        // Returns the value at the current position. Requires Valid().
        const std::string& value() const;

        // Advance to the next entry. Requires Valid().
        void Next();

        // Position at the first entry in the Memtable.
        void SeekToFirst();

        // Position at the first entry with key >= target.
        void Seek(const InternalKey& target);

    private:
        Table::Iterator iter_;
    };

    // Returns an iterator over this Memtable.
    Iterator NewIterator() const;

    // ── Write interface ───────────────────────────────────────────────────────

    // Insert a live value for (row, col, ts).
    // If an entry with the same (row, col, ts, kTypeValue) already exists,
    // its value is updated in-place (SkipList upsert semantics).
    void Put(const std::string& row,
             const std::string& col,
             int64_t            ts,
             const std::string& value);

    // Insert a deletion tombstone for (row, col, ts).
    // Does not remove any existing entries — the tombstone shadows them
    // during reads. The compactor discards both during SSTable merging.
    void Delete(const std::string& row,
                const std::string& col,
                int64_t            ts);

    // ── Read interface ────────────────────────────────────────────────────────

    // Look up the newest entry for (row, col).
    // Seeks to (row, col, INT64_MAX, kTypeValue) — the highest possible
    // key for that row/col — so the first hit is always the newest version.
    //
    // Returns:
    //   kFound    — value_out is populated with the cell value
    //   kDeleted  — newest entry is a tombstone; value_out is unchanged
    //   kNotFound — no entry for (row, col) in this Memtable
    GetResult Get(const std::string& row,
                  const std::string& col,
                  std::string&       value_out) const;

    // ── Size tracking ─────────────────────────────────────────────────────────

    // Returns total bytes allocated from the Arena.
    // Used by the Tablet to decide when to freeze and flush this Memtable.
    size_t ApproximateSize() const;

private:

    Arena  arena_;   // owns all node memory
    Table  table_;   // sorted store; uses arena_
};


#endif 
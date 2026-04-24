#include "memtable.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Implements the Memtable write and read interface.
// All data lives in the SkipList, which allocates nodes from the Arena.
// ─────────────────────────────────────────────────────────────────────────────


// ── Constructor ───────────────────────────────────────────────────────────────
// Arena is default-constructed (empty slab allocator).
// SkipList is constructed with InternalKeyComparator and a pointer to our
// Arena. Order matters — arena_ must be initialised before table_.
Memtable::Memtable()
    : arena_(),
      table_(InternalKeyComparator{}, &arena_)
{}


// ── Put ───────────────────────────────────────────────────────────────────────
// Creates a new version of (row, col) at the given timestamp.
// If the exact same (row, col, ts, kTypeValue) key already exists,
// the SkipList upserts the value in-place.
void Memtable::Put(const std::string& row,
                   const std::string& col,
                   int64_t            ts,
                   const std::string& value)
{
    InternalKey key(row, col, ts, kTypeValue);
    table_.Insert(key, value);
}


// ── Delete ────────────────────────────────────────────────────────────────────
// Inserts a tombstone for (row, col, ts).
// Does not remove any existing entries — the tombstone shadows them during
// reads. Old versions and the tombstone itself are pruned during compaction.
void Memtable::Delete(const std::string& row,
                      const std::string& col,
                      int64_t            ts)
{
    InternalKey key(row, col, ts, kTypeDeletion);
    table_.Insert(key, "");  // tombstone carries no value
}


// ── Get ───────────────────────────────────────────────────────────────────────
// Seeks to (row, col, INT64_MAX, kTypeValue) — the highest possible key for
// that (row, col) pair. Because timestamps sort descending, the first entry
// the iterator lands on is always the newest version.
//
// Three outcomes:
//   kFound    — iterator hit a live value for (row, col)
//   kDeleted  — newest entry is a tombstone
//   kNotFound — no entry exists for (row, col) in this Memtable
GetResult Memtable::Get(const std::string& row,
                        const std::string& col,
                        std::string&       value_out) const
{
    // Construct a seek key at the very top of the (row, col) range.
    // INT64_MAX timestamp + kTypeValue ensures we land at or before
    // the newest real entry for this (row, col).
    InternalKey seek_key(row, col, INT64_MAX, kTypeValue);

    Table::Iterator iter(&table_);
    iter.Seek(seek_key);

    if (!iter.Valid()) {
        return GetResult::kNotFound;
    }

    const InternalKey& found = iter.Key();

    // Check that the iterator landed on the correct (row, col).
    // If the seek overshot into a different row or col, there is no entry.
    if (found.row != row || found.col != col) {
        return GetResult::kNotFound;
    }

    // The newest entry for (row, col) is a tombstone — cell was deleted.
    if (found.type == kTypeDeletion) {
        return GetResult::kDeleted;
    }

    // Live value found.
    value_out = iter.Value();
    return GetResult::kFound;
}


// ── ApproximateSize ───────────────────────────────────────────────────────────
// Delegates directly to Arena::MemoryUsage().
// The Tablet uses this to decide when to freeze and flush this Memtable.
size_t Memtable::ApproximateSize() const {
    return arena_.MemoryUsage();
}


// ── Iterator ──────────────────────────────────────────────────────────────────
Memtable::Iterator::Iterator(const Memtable* mem)
    : iter_(&mem->table_)
{}

bool Memtable::Iterator::Valid() const {
    return iter_.Valid();
}

const InternalKey& Memtable::Iterator::key() const {
    return iter_.Key();
}

const std::string& Memtable::Iterator::value() const {
    return iter_.Value();
}

void Memtable::Iterator::Next() {
    iter_.Next();
}

void Memtable::Iterator::SeekToFirst() {
    iter_.SeekToFirst();
}

void Memtable::Iterator::Seek(const InternalKey& target) {
    iter_.Seek(target);
}

Memtable::Iterator Memtable::NewIterator() const {
    return Iterator(this);
}

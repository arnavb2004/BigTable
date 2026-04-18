#ifndef SKIPLIST_HPP
#include "skiplist.hpp"
#endif

#include <cstdlib>  // rand()
#include <cstring>  // memset (used in NewNode)
#include <new>      // placement new
#include <vector>
#include <random>

// ─────────────────────────────────────────────────────────────────────────────
// NewNode
//
// Lays out memory as:
//   [ Node header (k, v, next_[0]) ][ next_[1] ] ... [ next_[height-1] ]
//
// The first next_ slot is baked into the struct; each extra level needs one
// more std::atomic<Node*> sizeof.  We use placement-new so the Node
// constructor runs on properly-aligned memory.
// ─────────────────────────────────────────────────────────────────────────────
template<typename key, typename value, typename Comparator>
typename SkipList<key, value, Comparator>::Node*
SkipList<key, value, Comparator>::NewNode(const key& k, const value& v, int height)
{
    // Total bytes = base Node size + (height-1) extra atomic pointer slots.
    const size_t extra = (height - 1) * sizeof(std::atomic<Node*>);
    const size_t total = sizeof(Node) + extra;

    // Arena::AllocateAligned always uses max(sizeof(void*), 8) = 8-byte alignment,
    // which is sufficient for Node on all target platforms (alignof(Node) <= 8).
    char* mem = arena_->AllocateAligned(total);

    // Placement-new: run the Node constructor in our Arena memory.
    Node* node = new (mem) Node(k, v);

    // Zero-initialise all next_ pointers (including the extra ones).
    // std::atomic is not trivially zero-initialised in all compilers,
    // so we explicitly store nullptr into each slot.
    for (int i = 0; i < height; ++i) {
        node->SetNextRelaxed(i, nullptr);
    }
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// RandomHeight
//
// Geometric distribution with p = kBranchProb (0.25).
// Expected height ≈ 1 / (1 - p) ≈ 1.33.  Very few nodes exceed height 4;
// reaching kMaxHeight (12) is extremely rare.
// ─────────────────────────────────────────────────────────────────────────────
template<typename key, typename value, typename Comparator>
int SkipList<key, value, Comparator>::RandomHeight()
{
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    
    int height = 1;
    while (height < kMaxHeight && dist(rng) < kBranchProb) {
        ++height;
    }
    return height;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
template<typename key, typename value, typename Comparator>
SkipList<key, value, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(key{}, value{}, kMaxHeight)),
      max_height_(1)
{
    // head_ is a sentinel — its key is never compared against user keys
    // during a Search that starts from head_->Next(level).
    // All next_ pointers are already nullptr (set in NewNode).
}

// ─────────────────────────────────────────────────────────────────────────────
// FindGreaterOrEqual
//
// Returns the first node whose key >= k.
// If prev != nullptr, fills prev[level] with the last node on each level
// whose key < k — i.e. the splice points needed by Insert.
//
// This is the core traversal and is called by both Insert and Search.
// ─────────────────────────────────────────────────────────────────────────────
template<typename key, typename value, typename Comparator>
typename SkipList<key, value, Comparator>::Node*
SkipList<key, value, Comparator>::FindGreaterOrEqual(const key& k, Node** prev) const
{
    Node* current = head_;
    int   level   = max_height_.load(std::memory_order_relaxed) - 1;

    while (true) {
        Node* next = current->Next(level);

        if (KeyIsLessThan(next, k)) {
            // next->key < k: advance forward on this level.
            current = next;
        } else {
            // next is nullptr OR next->key >= k: record splice point and drop.
            if (prev != nullptr) {
                prev[level] = current;
            }
            if (level == 0) {
                return next; // next is either nullptr or the first node >= k
            }
            --level;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FindLast
// ─────────────────────────────────────────────────────────────────────────────
template<typename key, typename value, typename Comparator>
typename SkipList<key, value, Comparator>::Node*
SkipList<key, value, Comparator>::FindLast() const
{
    Node* current = head_;
    int   level   = max_height_.load(std::memory_order_relaxed) - 1;

    while (true) {
        Node* next = current->Next(level);
        if (next == nullptr) {
            if (level == 0) return (current == head_) ? nullptr : current;
            --level;
        } else {
            current = next;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Insert
//
// Steps:
//   1. Walk the list to find splice points (prev[]).
//   2. If the key already exists, atomically update its value and return.
//   3. Otherwise draw a random height, extend max_height_ if needed,
//      allocate a new Node, and splice it in from bottom to top.
//
// Writer serialisation: the caller must hold a mutex; no internal locking.
// ─────────────────────────────────────────────────────────────────────────────
template<typename key, typename value, typename Comparator>
void SkipList<key, value, Comparator>::Insert(const key& k, const value& v)
{
    // prev[i] will hold the last node on level i whose key < k.
    Node* prev[kMaxHeight];

    Node* existing = FindGreaterOrEqual(k, prev);

    // If key already exists, update value in-place (upsert semantics).
    if (existing != nullptr && KeyCompare(existing->k, k) == 0) {
        existing->v = v;   // serialised by caller's mutex
        return;
    }

    // New key: allocate a node and wire it in.
    const int height = RandomHeight();
    const int cur_max = max_height_.load(std::memory_order_relaxed);

    if (height > cur_max) {
        // Levels above cur_max have no prior nodes — head_ is the predecessor.
        for (int i = cur_max; i < height; ++i) {
            prev[i] = head_;
        }
        // Publish the new height.  Concurrent readers will see at most the old
        // height until this store; that is safe because the new levels still
        // point to nullptr at this point.
        max_height_.store(height, std::memory_order_relaxed);
    }

    Node* new_node = NewNode(k, v, height);

    // Splice from level 0 up to height-1.
    // We use relaxed stores for next_ here because the release store into
    // prev[0]->next_[0] acts as the publication fence for the whole node.
    for (int i = 0; i < height; ++i) {
        // new_node->next_[i] = prev[i]->next_[i]
        new_node->SetNextRelaxed(i, prev[i]->NextRelaxed(i));
    }

    // Now publish: use release semantics so that readers see the fully
    // initialised node once they follow this pointer.
    for (int i = 0; i < height; ++i) {
        prev[i]->SetNext(i, new_node);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Search
//
// Uses FindGreaterOrEqual (prev = nullptr → read-only path, no splice needed).
// Returns true and fills `v` if an exact match is found.
// ─────────────────────────────────────────────────────────────────────────────
template<typename key, typename value, typename Comparator>
bool SkipList<key, value, Comparator>::Search(const key& k, value& v) const
{
    Node* candidate = FindGreaterOrEqual(k, nullptr);

    if (candidate != nullptr && KeyCompare(candidate->k, k) == 0) {
        v = candidate->v;   // serialised by acquire on next_ pointer
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Iterator implementation
// ─────────────────────────────────────────────────────────────────────────────
template<typename key, typename value, typename Comparator>
SkipList<key, value, Comparator>::Iterator::Iterator(const SkipList* list)
    : list_(list), node_(nullptr)
{}

template<typename key, typename value, typename Comparator>
bool SkipList<key, value, Comparator>::Iterator::Valid() const
{
    return node_ != nullptr;
}

template<typename key, typename value, typename Comparator>
const key& SkipList<key, value, Comparator>::Iterator::Key() const
{
    assert(Valid());
    return node_->k;
}

template<typename key, typename value, typename Comparator>
const value& SkipList<key, value, Comparator>::Iterator::Value() const
{
    assert(Valid());
    return node_->v;
}

template<typename key, typename value, typename Comparator>
void SkipList<key, value, Comparator>::Iterator::Next()
{
    assert(Valid());
    node_ = node_->Next(0);
}

template<typename key, typename value, typename Comparator>
void SkipList<key, value, Comparator>::Iterator::SeekToFirst()
{
    node_ = list_->head_->Next(0);
}

template<typename key, typename value, typename Comparator>
void SkipList<key, value, Comparator>::Iterator::Seek(const key& target)
{
    node_ = list_->FindGreaterOrEqual(target, nullptr);
}
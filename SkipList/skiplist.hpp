#ifndef SKIPLIST_HPP
#define SKIPLIST_HPP

#include "../ArenaAllocator/arena.hpp"
#include <atomic>

template<typename key, typename value, typename Comparator>

class SkipList {
    public:
        // Pass the comparator and the memory pool
        explicit SkipList(Comparator cmp, Arena* const arena);

        void Insert(const key& k, const value& v);
        bool Search(const key& k, value& v) const;
        // void Delete(key k);
    private:
        struct Node {
            key const k;
            std::atomic<value> v;
            // A flexible array of atomic pointers to next nodes
            // We allocate the exact size needed from the Arena
            std::atomic<Node*> next[1];
            Node(const key& k, const value& v) : k(k), v(v) {}
        };

        static const int kMaxHeight = 12;

        Comparator const compare_;   // The "sorting rulebook"
        Arena* const arena_;         // Pointer to your custom allocator
        Node* const head_;           // Sentinel head node
        std::atomic<int> max_height_; // Current highest level in use

        Node* NewNode(const key& k, const value& v, int height);
        int RandomHeight();

        // Prevent copying to avoid memory ownership confusion
        SkipList(const SkipList&) = delete;
        void operator=(const SkipList&) = delete;
};

#include "skiplist.ipp" // Include the implementation of template methods
#endif
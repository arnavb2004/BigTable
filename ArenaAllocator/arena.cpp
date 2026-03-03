#include "arena.h"

static const int kBlockSize = 4096; // Standard 4KB page size

Arena::Arena() 
    : alloc_ptr_(nullptr), 
      alloc_bytes_remaining_(0), 
      memory_usage_(0) {
        /* Constructor initializes the arena with no allocated memory and zero usage.
        alloc_ptr_ is set to nullptr, indicating no current block.
        alloc_bytes_remaining_ is set to 0, meaning no space is available for allocation.
        
        alloc_ptr_ : Pointer to the next available byte in the current block.
        alloc_bytes_remaining_ : Number of bytes left in the current block.
        memory_usage_ : Atomic counter tracking the total bytes allocated from the OS. */
}

char* Arena::Allocate(size_t bytes) {
    /* Returns pointer to memory if sufficient memory is available in the arena.
    Else it goes to AllocateFallBack function.
    
    bytes : The number of raw bytes the user wants to allocate. */
    if (bytes <= 0) return nullptr;

    if (bytes <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }
    return AllocateFallback(bytes);
}

char* Arena::AllocateFallback(size_t bytes) {
    /* If the requested memory size is more than 1/4th of our arena size, we allocate a separate arena of size bytes.
    Else we start a new standard arena and allocate memory accordingly.
    
    bytes : The number of bytes that didn't fit in the previous block */
    if (bytes > kBlockSize / 4) {
        // Object is huge, allocate a dedicated block for it
        return AllocateNewBlock(bytes);
    }
    // Otherwise, start a fresh standard-sized block
    alloc_ptr_ = AllocateNewBlock(kBlockSize);
    alloc_bytes_remaining_ = kBlockSize;

    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
    /* Allocate a new standard arena. 
    
    block_bytes : The exact size of the new memory chunk to request from the heap. */
    // 1. Request a raw chunk from the heap
    char* result = new char[block_bytes];
    
    // 2. Keep track of it so we can delete it in the destructor
    blocks_.push_back(result);
    
    // 3. Update the total memory counter (using relaxed memory order is fine here)
    memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);
    
    return result;
}

char* Arena::AllocateAligned(size_t bytes) {
    /* It ensures alignment of memory, here you want memory to start from multiple of 8's only. 
    
    bytes : The number of bytes to allocate, ensuring the starting pointer is aligned. */
    if (bytes <= 0) return nullptr;
    const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
    static_assert((align & (align - 1)) == 0, "Pointer size should be a power of 2");
    
    // Calculate how much padding is needed
    size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
    size_t slop = (current_mod == 0 ? 0 : align - current_mod);
    size_t needed = bytes + slop;

    if (needed <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_ + slop;
        alloc_ptr_ += needed;
        alloc_bytes_remaining_ -= needed;
        return result;
    }
    // Fallback doesn't need slop because new blocks are already aligned
    return AllocateFallback(bytes);
}

Arena::~Arena() {
    /* Destructor cleans up all allocated blocks. */
    for (char* block : blocks_) {
        delete[] block;
    }
}
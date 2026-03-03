#include <vector>
#include <atomic>
#include <cstddef>

class Arena {
public:
    Arena();
    ~Arena();

    // Standard bump allocation
    char* Allocate(size_t bytes);

    // Aligned allocation (crucial for performance)
    char* AllocateAligned(size_t bytes);

    // Returns total memory allocated from the system
    size_t MemoryUsage() const {
        return memory_usage_.load(std::memory_order_relaxed);
    }

    // Disable copying
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

private:
    char* AllocateFallback(size_t bytes);
    char* AllocateNewBlock(size_t block_bytes);

    // Allocation state
    char* alloc_ptr_;
    size_t alloc_bytes_remaining_;

    // Array of new[] allocated memory blocks
    std::vector<char*> blocks_;

    // Total memory usage
    std::atomic<size_t> memory_usage_;
};
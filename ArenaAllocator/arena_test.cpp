#include <iostream>
#include <cassert>
#include <vector>
#include "arena.hpp"

void TestBasic() {
    Arena arena;
    char* ptr = arena.Allocate(100);
    assert(ptr != nullptr);
    assert(arena.MemoryUsage() >= 100);
    std::cout << "TestBasic Passed" << std::endl;
}

void TestAlignment() {
    Arena arena;
    const int N = 100;
    std::vector<size_t> sizes = {1, 10, 3, 7, 16, 1, 1};
    
    for (size_t sz : sizes) {
        char* ptr = arena.AllocateAligned(sz);
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        
        // Check if address is multiple of 8
        if (addr % 8 != 0) {
            std::cerr << "Alignment Error: Address " << addr << " not divisible by 8" << std::endl;
            exit(1);
        }
    }
    std::cout << "TestAlignment Passed (All addresses divisible by 8)" << std::endl;
}

void TestHugeAllocation() {
    Arena arena;
    // Standard block is 4KB. Let's ask for 10KB.
    char* ptr = arena.Allocate(10000);
    assert(ptr != nullptr);
    assert(arena.MemoryUsage() >= 10000);
    std::cout << "TestHugeAllocation Passed" << std::endl;
}

void TestStress() {
    Arena arena;
    for (int i = 1; i < 1000; ++i) {
        size_t s = (i % 128) + 1;
        char* ptr = arena.Allocate(s);
        
        if (ptr == nullptr) {
            std::cout << "Allocation failed at iteration " << i << " for size " << s << std::endl;
        }
        assert(ptr != nullptr);
    }
    std::cout << "TestStress Passed (1000 small allocations)" << std::endl;
}

int main() {
    std::cout << "Starting Arena Tests..." << std::endl;
    
    TestBasic();
    TestAlignment();
    TestHugeAllocation();
    TestStress();
    
    std::cout << "ALL TESTS PASSED!" << std::endl;
    return 0;
}
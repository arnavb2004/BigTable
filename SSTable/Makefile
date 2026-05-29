CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g

ifeq ($(OS),Windows_NT)
    TEST_BIN := sstable_test.exe
    RUN      := sstable_test.exe
    RM       := del /Q
else
    TEST_BIN := sstable_test
    RUN      := ./sstable_test
    RM       := rm -f
endif

DEPS_DIR  := ..
ARENA_OBJ := $(DEPS_DIR)/ArenaAllocator/arena.o
IK_OBJ    := $(DEPS_DIR)/MemTable/internal_key.o
MEM_OBJ   := $(DEPS_DIR)/MemTable/memtable.o

.PHONY: all test clean arena_dep mem_dep

all: $(TEST_BIN)

arena_dep:
	$(MAKE) -C $(DEPS_DIR)/ArenaAllocator arena.o

mem_dep: arena_dep
	$(MAKE) -C $(DEPS_DIR)/MemTable internal_key.o
	$(MAKE) -C $(DEPS_DIR)/MemTable memtable.o

sstable.o: sstable.cpp sstable.hpp mem_dep
	$(CXX) $(CXXFLAGS) -c sstable.cpp -o sstable.o

$(TEST_BIN): sstable_test.cpp sstable.o $(MEM_OBJ) $(IK_OBJ) $(ARENA_OBJ)
	$(CXX) $(CXXFLAGS) sstable_test.cpp sstable.o $(MEM_OBJ) $(IK_OBJ) $(ARENA_OBJ) -o $(TEST_BIN)

test: $(TEST_BIN)
	$(RUN)

clean:
	$(RM) sstable.o $(TEST_BIN)
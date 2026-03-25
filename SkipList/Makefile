CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g

# Detect Windows (MinGW sets OS=Windows_NT)
ifeq ($(OS),Windows_NT)
    CXXFLAGS  += -lpthread
    TEST_BIN  := skiplist_test.exe
    RUN       := skiplist_test.exe
    RM        := del /Q
else
    TEST_BIN  := skiplist_test
    RUN       := ./skiplist_test
    RM        := rm -f
endif

# Arena object lives one level up; build it first if needed
ARENA_DIR := ../ArenaAllocator
ARENA_OBJ := $(ARENA_DIR)/arena.o

# SkipList is header-only (hpp + ipp).
# skiplist.ipp is #included by skiplist.hpp — it is NOT a separately compiled
# file, so it must NOT appear as a Make target or recipe input.
# We list skiplist.hpp as the dependency; hpp pulls in ipp automatically.
HEADERS := skiplist.hpp skiplist.ipp

.PHONY: all test clean arena_dep

all: $(TEST_BIN)

# Ensure the arena object is up to date before linking
arena_dep:
	$(MAKE) -C $(ARENA_DIR) arena.o

# Build the test binary.
# The only source file compiled is skiplist_test.cpp.
# arena.o is linked in to resolve Arena::* symbols.
$(TEST_BIN): skiplist_test.cpp $(HEADERS) arena_dep
	$(CXX) $(CXXFLAGS) skiplist_test.cpp $(ARENA_OBJ) -o $(TEST_BIN)

# Run the tests
test: $(TEST_BIN)
	$(RUN)

clean:
	$(RM) $(TEST_BIN)
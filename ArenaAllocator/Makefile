CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g

ifeq ($(OS),Windows_NT)
    TEST_BIN := arena_test.exe
    RUN      := arena_test.exe
    RM       := del /Q
else
    TEST_BIN := arena_test
    RUN      := ./arena_test
    RM       := rm -f
endif

.PHONY: all test clean

all: $(TEST_BIN)

# Compile arena.cpp into a reusable object file.
# SkipList/Makefile links against this via ../ArenaAllocator/arena.o
arena.o: arena.cpp arena.hpp
	$(CXX) $(CXXFLAGS) -c arena.cpp -o arena.o

# Build the test binary
$(TEST_BIN): arena_test.cpp arena.o
	$(CXX) $(CXXFLAGS) arena_test.cpp arena.o -o $(TEST_BIN)

test: $(TEST_BIN)
	$(RUN)

clean:
	$(RM) arena.o $(TEST_BIN)
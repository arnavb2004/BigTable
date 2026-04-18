CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g

ifeq ($(OS),Windows_NT)
    TEST_BIN := internal_key_test.exe
    RUN      := internal_key_test.exe
    RM       := del /Q
else
    TEST_BIN := internal_key_test
    RUN      := ./internal_key_test
    RM       := rm -f
endif

.PHONY: all test clean

all: $(TEST_BIN)

# Compile internal_key.cpp into a reusable object file.
# Memtable/Makefile will link against this via ../InternalKey/internal_key.o
internal_key.o: internal_key.cpp internal_key.hpp
	$(CXX) $(CXXFLAGS) -c internal_key.cpp -o internal_key.o

# Build the test binary
$(TEST_BIN): internal_key_test.cpp internal_key.o
	$(CXX) $(CXXFLAGS) internal_key_test.cpp internal_key.o -o $(TEST_BIN)

test: $(TEST_BIN)
	$(RUN)

clean:
	$(RM) internal_key.o $(TEST_BIN)
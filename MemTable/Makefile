CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g

ifeq ($(OS),Windows_NT)
    INTERNAL_KEY_BIN := internal_key_test.exe
    MEMTABLE_BIN     := memtable_test.exe
    RUN_IK           := internal_key_test.exe
    RUN_MT           := memtable_test.exe
    RM               := del /Q
else
    INTERNAL_KEY_BIN := internal_key_test
    MEMTABLE_BIN     := memtable_test
    RUN_IK           := ./internal_key_test
    RUN_MT           := ./memtable_test
    RM               := rm -f
endif

.PHONY: all test clean

all: $(INTERNAL_KEY_BIN) $(MEMTABLE_BIN)

# ── ArenaAllocator dependency ─────────────────────────────────────────────────
../ArenaAllocator/arena.o:
	$(MAKE) -C ../ArenaAllocator arena.o

# ── InternalKey ───────────────────────────────────────────────────────────────

# Compile internal_key.cpp into a reusable object file.
# memtable.o links against this.
internal_key.o: internal_key.cpp internal_key.hpp
	$(CXX) $(CXXFLAGS) -c internal_key.cpp -o internal_key.o

# Build the InternalKey test binary
$(INTERNAL_KEY_BIN): internal_key_test.cpp internal_key.o
	$(CXX) $(CXXFLAGS) internal_key_test.cpp internal_key.o -o $(INTERNAL_KEY_BIN)

# ── Memtable ──────────────────────────────────────────────────────────────────

# Compile memtable.cpp into a reusable object file.
memtable.o: memtable.cpp memtable.hpp internal_key.o
	$(CXX) $(CXXFLAGS) -c memtable.cpp -o memtable.o

# Build the Memtable test binary
$(MEMTABLE_BIN): memtable_test.cpp memtable.o internal_key.o ../ArenaAllocator/arena.o
	$(CXX) $(CXXFLAGS) memtable_test.cpp memtable.o internal_key.o ../ArenaAllocator/arena.o -o $(MEMTABLE_BIN)

# ── Targets ───────────────────────────────────────────────────────────────────

test: all
	@echo ""
	@echo "======================================"
	@echo "  Running InternalKey tests"
	@echo "======================================"
	$(RUN_IK)
	@echo ""
	@echo "======================================"
	@echo "  Running Memtable tests"
	@echo "======================================"
	$(RUN_MT)

clean:
	$(RM) internal_key.o memtable.o $(INTERNAL_KEY_BIN) $(MEMTABLE_BIN)
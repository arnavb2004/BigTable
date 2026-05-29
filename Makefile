# ─────────────────────────────────────────────────────────────────────────────
# BigTable/Makefile  —  root-level orchestrator
#
# Usage:
#   make              → build everything
#   make test         → build + run all test suites
#   make arena        → build ArenaAllocator only
#   make skiplist     → build SkipList (+ Arena dependency) only
#   make memtable     → build MemTable (+ all dependencies) only
#   make sstable 	  → build SSTable (+ all dependencies) only
#   make clean        → remove all build artifacts
# ─────────────────────────────────────────────────────────────────────────────

# Use mingw32-make on Windows, make elsewhere
ifeq ($(OS),Windows_NT)
    MAKE := mingw32-make
endif

.PHONY: all test arena skiplist memtable clean

all: arena skiplist memtable

arena:
	$(MAKE) -C ArenaAllocator

skiplist:
	$(MAKE) -C SkipList

memtable:
	$(MAKE) -C MemTable

sstable:
	$(MAKE) -C SSTable

test: all
	@echo ""
	@echo "======================================"
	@echo "  Running ArenaAllocator tests"
	@echo "======================================"
	$(MAKE) -C ArenaAllocator test

	@echo ""
	@echo "======================================"
	@echo "  Running SkipList tests"
	@echo "======================================"
	$(MAKE) -C SkipList test

	@echo ""
	@echo "======================================"
	@echo "  Running MemTable tests"
	@echo "======================================"
	$(MAKE) -C MemTable test

	@echo ""
	@echo "======================================"
	@echo "  Running SSTable tests"
	@echo "======================================"
	$(MAKE) -C SSTable test

clean:
	$(MAKE) -C ArenaAllocator clean
	$(MAKE) -C SkipList clean
	$(MAKE) -C MemTable clean
	$(MAKE) -C SSTable clean
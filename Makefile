# ─────────────────────────────────────────────────────────────────────────────
# BigTable/Makefile  —  root-level orchestrator
#
# Usage:
#   make              → build everything
#   make test         → build + run all test suites
#   make arena        → build ArenaAllocator only
#   make skiplist     → build SkipList (+ Arena dependency) only
#   make clean        → remove all build artifacts
# ─────────────────────────────────────────────────────────────────────────────

# Use mingw32-make on Windows, make elsewhere
ifeq ($(OS),Windows_NT)
    MAKE := mingw32-make
endif

.PHONY: all test arena skiplist clean

all: arena skiplist

arena:
	$(MAKE) -C ArenaAllocator

skiplist:
	$(MAKE) -C SkipList

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

clean:
	$(MAKE) -C ArenaAllocator clean
	$(MAKE) -C SkipList clean
CXX = g++
CXXFLAGS = -std=c++17 -Wall

# The "Library" output
LIB_OBJ = arena.o
# The "Test" output
TEST_EXE = arena_test

all: $(TEST_EXE)
	./$(TEST_EXE)

# This part is like your cc_library 'srcs'
$(LIB_OBJ): arena.cpp arena.h
	$(CXX) $(CXXFLAGS) -c arena.cpp -o $(LIB_OBJ)

# This part is like your cc_test 'deps'
$(TEST_EXE): arena_test.cpp $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) arena_test.cpp $(LIB_OBJ) -o $(TEST_EXE)

clean:
	rm -f *.o $(TEST_EXE)
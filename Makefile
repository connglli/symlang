CXX = g++
CXXFLAGS = -std=c++20 -Iinclude -Wall -Wextra

COMMON_SRCS = src/frontend/lexer.cpp src/frontend/parser.cpp src/frontend/ast_dumper.cpp \
              src/analysis/cfgbuilder.cpp src/analysis/definite_init.cpp \
              src/frontend/typechecker.cpp src/frontend/semchecker.cpp

TEST_SRCS = src/test/sym_test.cpp
INTERP_SRCS = src/symiri.cpp src/interp/interpreter.cpp

COMMON_OBJS = $(COMMON_SRCS:.cpp=.o)
TEST_OBJS = $(TEST_SRCS:.cpp=.o)
INTERP_OBJS = $(INTERP_SRCS:.cpp=.o)

TARGET_TEST = sym_test
TARGET_INTERP = symiri

all: $(TARGET_TEST) $(TARGET_INTERP)

$(TARGET_TEST): $(COMMON_OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(TARGET_INTERP): $(COMMON_OBJS) $(INTERP_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(COMMON_OBJS) $(TEST_OBJS) $(INTERP_OBJS) $(TARGET_TEST) $(TARGET_INTERP)

test: $(TARGET_TEST) $(TARGET_INTERP)
	python3 run_tests.py test/lexer ./$(TARGET_TEST)
	python3 run_tests.py test/parser ./$(TARGET_TEST)
	python3 run_tests.py test/cfgbuilder ./$(TARGET_TEST)
	python3 run_tests.py test/typechecker ./$(TARGET_TEST)
	python3 run_tests.py test/semchecker ./$(TARGET_TEST)
	python3 run_tests.py test/interp ./$(TARGET_INTERP)

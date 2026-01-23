CXX = g++
CXXFLAGS = -std=c++20 -Iinclude -Wall -Wextra

COMMON_SRCS = src/frontend/lexer.cpp src/frontend/parser.cpp src/frontend/ast_dumper.cpp \
              src/analysis/cfgbuilder.cpp src/analysis/definite_init.cpp \
              src/frontend/typechecker.cpp src/frontend/semchecker.cpp \
              src/analysis/pass_manager.cpp src/analysis/reachability.cpp \
              src/analysis/unused_name.cpp

TEST_SRCS =
INTERP_SRCS = src/symiri.cpp src/interp/interpreter.cpp
COMPILER_SRCS = src/symirc.cpp src/backend/c_backend.cpp

COMMON_OBJS = $(COMMON_SRCS:.cpp=.o)
TEST_OBJS = $(TEST_SRCS:.cpp=.o)
INTERP_OBJS = $(INTERP_SRCS:.cpp=.o)
COMPILER_OBJS = $(COMPILER_SRCS:.cpp=.o)

TARGET_INTERP = symiri
TARGET_COMPILER = symirc

all: $(TARGET_INTERP) $(TARGET_COMPILER)

$(TARGET_INTERP): $(COMMON_OBJS) $(INTERP_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(TARGET_COMPILER): $(COMMON_OBJS) $(COMPILER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(COMMON_OBJS) $(TEST_OBJS) $(INTERP_OBJS) $(COMPILER_OBJS) $(TARGET_INTERP) $(TARGET_COMPILER)

test: $(TARGET_INTERP)
	python3 run_tests.py test/lexer ./$(TARGET_INTERP) --check
	python3 run_tests.py test/parser ./$(TARGET_INTERP) --check
	python3 run_tests.py test/cfgbuilder ./$(TARGET_INTERP) --check
	python3 run_tests.py test/typechecker ./$(TARGET_INTERP) --check
	python3 run_tests.py test/semchecker ./$(TARGET_INTERP) --check
	python3 run_tests.py test/interp ./$(TARGET_INTERP)

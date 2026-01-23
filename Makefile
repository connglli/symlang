CXX = g++
CXXFLAGS = -std=c++20 -Iinclude -Ibitwuzla/include -Wall -Wextra
LDFLAGS = -Lbitwuzla/lib/x86_64-linux-gnu -lbitwuzla -lbitwuzlals -lbitwuzlabv -lbitwuzlabb -lgmp

COMMON_SRCS = src/frontend/lexer.cpp src/frontend/parser.cpp src/frontend/ast_dumper.cpp \
              src/analysis/cfgbuilder.cpp src/analysis/definite_init.cpp \
              src/frontend/typechecker.cpp src/frontend/semchecker.cpp \
              src/analysis/pass_manager.cpp src/analysis/reachability.cpp \
              src/analysis/unused_name.cpp

TEST_SRCS =
INTERP_SRCS = src/symiri.cpp src/interp/interpreter.cpp
COMPILER_SRCS = src/symirc.cpp src/backend/c_backend.cpp
SOLVER_SRCS = src/symirsolve.cpp src/solver/solver.cpp

COMMON_OBJS = $(COMMON_SRCS:.cpp=.o)
TEST_OBJS = $(TEST_SRCS:.cpp=.o)
INTERP_OBJS = $(INTERP_SRCS:.cpp=.o)
COMPILER_OBJS = $(COMPILER_SRCS:.cpp=.o)
SOLVER_OBJS = $(SOLVER_SRCS:.cpp=.o)

TARGET_INTERP = symiri
TARGET_COMPILER = symirc
TARGET_SOLVER = symirsolve

PY := $(shell command -v python3 >/dev/null 2>&1 && echo python3 || echo python)

.PHONY: all clean test

all: $(TARGET_INTERP) $(TARGET_COMPILER) $(TARGET_SOLVER)

$(TARGET_INTERP): $(COMMON_OBJS) $(INTERP_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_COMPILER): $(COMMON_OBJS) $(COMPILER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_SOLVER): $(COMMON_OBJS) $(SOLVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(COMMON_OBJS) $(TEST_OBJS) $(INTERP_OBJS) $(COMPILER_OBJS) $(SOLVER_OBJS) $(TARGET_INTERP) $(TARGET_COMPILER) $(TARGET_SOLVER)
test: $(TARGET_INTERP) $(TARGET_COMPILER)
	$(PY) -m test.lib.run_tests test/lexer ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_tests test/parser ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_tests test/cfgbuilder ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_tests test/typechecker ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_tests test/semchecker ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_tests test/interp ./$(TARGET_INTERP)
	$(PY) -m test.lib.run_compiler_tests test/ ./$(TARGET_COMPILER)
	$(PY) -m test.lib.run_solver_tests test/solver ./$(TARGET_SOLVER)

CXX ?= $(shell command -v g++ || shell command -v clang++)
AR ?= ar
PY := $(shell command -v python3 || shell command -v python)

ifeq ($(CXX),)
  $(error "C++ compiler '$(CXX)' not found. Please install g++ or clang.")
endif
ifeq ($(AR),)
  $(error "Archiver '$(AR)' not found. Please install build-essential or binutils.")
endif
ifeq ($(PY),)
  $(error "Python not found. Please install Python or create a virtualenv first (e.g., 'python3 -m venv venv && source venv/bin/activate').")
endif

SOLVER ?= bitwuzla

CXXFLAGS = -std=c++20 -Iinclude -Wall -Wextra -O2
LDFLAGS =
ARFLAGS = rcs

# Coverage support
ifeq ($(COVERAGE), 1)
  CXXFLAGS += --coverage
  LDFLAGS += --coverage
endif

# Solver support
ifeq ($(SOLVER), bitwuzla)
  CXXFLAGS += -DUSE_BITWUZLA -Ibitwuzla/include
  LDFLAGS += -Lbitwuzla/lib/x86_64-linux-gnu -lbitwuzla -lbitwuzlals -lbitwuzlabv -lbitwuzlabb -lgmp
  SOLVER_SRCS += src/solver/bitwuzla_impl.cpp
  SOLVER_IMPL_OBJ = src/solver/bitwuzla_impl.o
else ifeq ($(SOLVER), alivesmt)
  CXXFLAGS += -DUSE_ALIVESMT -Ialivesmt/include
  LDFLAGS += -lz3
  ALIVESMT_SRCS = alivesmt/lib/ctx.cpp alivesmt/lib/expr.cpp alivesmt/lib/exprs.cpp alivesmt/lib/smt.cpp alivesmt/lib/solver.cpp alivesmt/lib/util.cpp
  SOLVER_SRCS += src/solver/alive_impl.cpp $(ALIVESMT_SRCS)
  SOLVER_IMPL_OBJ = src/solver/alive_impl.o $(ALIVESMT_SRCS:.cpp=.o)
else
  $(error "Unknown SOLVER: $(SOLVER). Supported: bitwuzla, alivesmt")
endif

COMMON_SRCS = src/frontend/lexer.cpp src/frontend/parser.cpp src/frontend/ast_dumper.cpp \
              src/frontend/sir_printer.cpp \
              src/analysis/cfgbuilder.cpp src/analysis/definite_init.cpp \
              src/frontend/typechecker.cpp src/frontend/semchecker.cpp \
              src/analysis/pass_manager.cpp src/analysis/reachability.cpp \
              src/analysis/unused_name.cpp src/analysis/type_utils.cpp \
              src/frontend/diagnostics.cpp

TEST_SRCS =
INTERP_SRCS = src/symiri.cpp src/interp/interpreter.cpp
COMPILER_SRCS = src/symirc.cpp src/backend/c_backend.cpp src/backend/wasm_backend.cpp
SOLVER_MAIN_SRCS = src/symirsolve.cpp src/solver/solver.cpp
SOLVER_ALL_SRCS = $(SOLVER_MAIN_SRCS) $(SOLVER_SRCS)

COMMON_OBJS = $(COMMON_SRCS:.cpp=.o)
TEST_OBJS = $(TEST_SRCS:.cpp=.o)
INTERP_OBJS = $(INTERP_SRCS:.cpp=.o)
COMPILER_OBJS = $(COMPILER_SRCS:.cpp=.o)
SOLVER_OBJS = $(SOLVER_MAIN_SRCS:.cpp=.o) $(SOLVER_IMPL_OBJ)

TARGET_INTERP = symiri
TARGET_COMPILER = symirc
TARGET_SOLVER = symirsolve

BUILD_DIR = build
BIN_DIR = $(BUILD_DIR)/bin
LIB_DIR = $(BUILD_DIR)/lib
INC_DIR = $(BUILD_DIR)/include

LIB_NAME = libsymir.a
LIBRARY_OBJS = $(COMMON_OBJS) \
               src/interp/interpreter.o \
               src/backend/c_backend.o \
               src/backend/wasm_backend.o \
               src/solver/solver.o \
               $(SOLVER_IMPL_OBJ)

.PHONY: all clean test build

all: $(TARGET_INTERP) $(TARGET_COMPILER) $(TARGET_SOLVER)

$(TARGET_INTERP): $(COMMON_OBJS) $(INTERP_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_COMPILER): $(COMMON_OBJS) $(COMPILER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_SOLVER): $(COMMON_OBJS) $(SOLVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build: all $(LIB_DIR)/$(LIB_NAME)
	mkdir -p $(BIN_DIR) $(INC_DIR)
	cp $(TARGET_INTERP) $(TARGET_COMPILER) $(TARGET_SOLVER) $(BIN_DIR)/
	cp -r include/* $(INC_DIR)/

$(LIB_DIR)/$(LIB_NAME): $(LIBRARY_OBJS)
	mkdir -p $(LIB_DIR)
	$(AR) $(ARFLAGS) $@ $^

clean:
	rm -f $(COMMON_OBJS) $(TEST_OBJS) $(INTERP_OBJS) $(COMPILER_OBJS) $(SOLVER_OBJS) $(TARGET_INTERP) $(TARGET_COMPILER) $(TARGET_SOLVER)
	rm -rf $(BUILD_DIR)
	find . -name "*.gcno" -delete
	find . -name "*.gcda" -delete
	find . -name "*.gcov" -delete

test: $(TARGET_INTERP) $(TARGET_COMPILER) $(TARGET_SOLVER)
	$(PY) -m test.lib.run_interp_tests test/lexer ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_interp_tests test/parser ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_interp_tests test/cfgbuilder ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_interp_tests test/typechecker ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_interp_tests test/semchecker ./$(TARGET_INTERP) --check
	$(PY) -m test.lib.run_interp_tests test/interp ./$(TARGET_INTERP)
	$(PY) -m test.lib.run_interp_tests test/complex ./$(TARGET_INTERP)
	$(PY) -m test.lib.run_compiler_tests test/ ./$(TARGET_COMPILER) --target c
	$(PY) -m test.lib.run_compiler_tests test/ ./$(TARGET_COMPILER) --target wasm
	$(PY) -m test.lib.run_solver_tests test/solver ./$(TARGET_SOLVER) ./$(TARGET_INTERP)
	$(PY) -m test.lib.run_solver_tests test/sample ./$(TARGET_SOLVER) ./$(TARGET_INTERP)
	$(PY) -m test.lib.run_example_tests examples ./$(TARGET_SOLVER) ./$(TARGET_INTERP)

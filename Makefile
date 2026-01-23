CXX = g++
CXXFLAGS = -std=c++20 -Iinclude -Wall -Wextra

SRCS = src/frontend/lexer.cpp src/frontend/parser.cpp src/frontend/ast_dumper.cpp \
       src/analysis/cfgbuilder.cpp src/analysis/definite_init.cpp \
       src/frontend/typechecker.cpp src/test/sym_test.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = sym_test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

test: $(TARGET)
	python3 run_tests.py test ./$(TARGET)

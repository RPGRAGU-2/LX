CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Isrc
LDFLAGS  = -lpthread
SRCS     = src/lx_value.cpp src/lx_lexer.cpp src/lx_parser.cpp \
           src/lx_bytecode.cpp src/lx_compiler.cpp \
           src/lx_vm.cpp src/lx_import.cpp src/main.cpp
TARGET   = lx

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

test: $(TARGET)
	./lx run tests/test_oop.lx
	./lx run tests/test_errors.lx
	./lx run tests/test_files.lx
	./lx run tests/test_tasks.lx

clean:
	rm -f $(TARGET) tests/*.lxc

.PHONY: all test clean

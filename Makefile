CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SRC      := src/main.cpp src/daemon.cpp src/container.cpp src/common.cpp
BIN      := build/runtime

all: $(BIN)

$(BIN): $(SRC) $(wildcard src/*.hpp)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

rootfs:
	bash scripts/get_rootfs.sh

test: $(BIN)
	bash tests/test_container.sh
	bash tests/test_daemon.sh

clean:
	rm -rf build

.PHONY: all rootfs test clean

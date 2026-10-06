CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -pthread
INCLUDES = -Iinclude

SRC_DIR = src
BUILD_DIR = build
TEST_DIR = tests
BENCH_DIR = benchmarks

TARGET = $(BUILD_DIR)/vector-vm-engine
TEST_TARGET = $(BUILD_DIR)/run_tests
BENCH_TARGET = $(BUILD_DIR)/run_benchmarks

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))

TEST_SRCS = $(wildcard $(TEST_DIR)/*.cpp)
BENCH_SRCS = $(wildcard $(BENCH_DIR)/*.cpp)

.PHONY: all build test clean benchmark directories

all: directories build

directories:
	@mkdir -p $(BUILD_DIR)

build: directories $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

test: directories
	@if [ -d "$(TEST_DIR)" ] && [ -n "$$($(wildcard $(TEST_DIR)/*.cpp))" ]; then \
		$(CXX) $(CXXFLAGS) $(INCLUDES) $(TEST_SRCS) $(filter-out $(BUILD_DIR)/main.o, $(OBJS)) -o $(TEST_TARGET); \
		$(TEST_TARGET); \
	else \
		echo "No tests found in $(TEST_DIR). Skipping."; \
	fi

benchmark: directories
	@if [ -d "$(BENCH_DIR)" ] && [ -n "$$($(wildcard $(BENCH_DIR)/*.cpp))" ]; then \
		$(CXX) $(CXXFLAGS) $(INCLUDES) $(BENCH_SRCS) $(filter-out $(BUILD_DIR)/main.o, $(OBJS)) -o $(BENCH_TARGET); \
		$(BENCH_TARGET); \
	else \
		echo "No benchmarks found in $(BENCH_DIR). Skipping."; \
	fi

clean:
	@rm -rf $(BUILD_DIR)
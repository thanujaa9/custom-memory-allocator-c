CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?= -pthread
BUILD_DIR := build

.PHONY: all test test-tsan test-asan benchmark clean

all: $(BUILD_DIR)/allocator_tests $(BUILD_DIR)/benchmark

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/allocator_tests: src/allocator.c tests/allocator_tests.c include/allocator.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) src/allocator.c tests/allocator_tests.c -o $@ $(LDFLAGS)

$(BUILD_DIR)/allocator_tests_tsan: src/allocator.c tests/allocator_tests.c include/allocator.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fsanitize=thread -g src/allocator.c tests/allocator_tests.c -o $@ $(LDFLAGS)

$(BUILD_DIR)/allocator_tests_asan: src/allocator.c tests/allocator_tests.c include/allocator.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g src/allocator.c tests/allocator_tests.c -o $@ $(LDFLAGS)

$(BUILD_DIR)/benchmark: src/allocator.c src/benchmark.c include/allocator.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) src/allocator.c src/benchmark.c -o $@ $(LDFLAGS)

test: $(BUILD_DIR)/allocator_tests
	./$(BUILD_DIR)/allocator_tests

test-tsan: $(BUILD_DIR)/allocator_tests_tsan
	./$(BUILD_DIR)/allocator_tests_tsan

test-asan: $(BUILD_DIR)/allocator_tests_asan
	./$(BUILD_DIR)/allocator_tests_asan

benchmark: $(BUILD_DIR)/benchmark
	./$(BUILD_DIR)/benchmark

clean:
	rm -rf $(BUILD_DIR)

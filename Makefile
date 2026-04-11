CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Iinclude -Ithird_party/unity/src -pthread
LDFLAGS := -pthread
VERSION := 1.0.0

BUILD_DIR := build
SRC_SOURCES := $(wildcard src/core/*.c) $(wildcard src/store/*.c) $(wildcard src/containers/*.c)
UNITY_SOURCES := third_party/unity/src/unity.c

TESTS_CORE := $(wildcard tests/core/*.c)
TESTS_STORE := $(wildcard tests/store/*.c)
TESTS_CONTAINERS := $(wildcard tests/containers/*.c)
TESTS_STRESS := $(wildcard tests/stress/*.c)

TEST_BINS = $(patsubst tests/core/%.c,$(BUILD_DIR)/test_core_%,$(TESTS_CORE)) \
            $(patsubst tests/store/%.c,$(BUILD_DIR)/test_store_%,$(TESTS_STORE)) \
            $(patsubst tests/containers/%.c,$(BUILD_DIR)/test_containers_%,$(TESTS_CONTAINERS))

STRESS_BINS = $(patsubst tests/stress/%.c,$(BUILD_DIR)/stress_%,$(TESTS_STRESS))

EXAMPLE_SOURCES := $(wildcard examples/*.c)
EXAMPLE_BINS := $(patsubst examples/%.c,$(BUILD_DIR)/%,$(EXAMPLE_SOURCES))

SANITIZE_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
ASAN_CFLAGS := -fsanitize=address -fno-omit-frame-pointer -g
UBSAN_CFLAGS := -fsanitize=undefined -fno-omit-frame-pointer -g
SANITIZE_LDFLAGS := -fsanitize=address,undefined

.PHONY: all test stress examples clean dirs
.PHONY: test-asan test-ubsan test-sanitize
.PHONY: stress-asan stress-ubsan stress-sanitize

all: dirs examples

dirs:
	mkdir -p $(BUILD_DIR)

test: dirs $(TEST_BINS)
	@total_tests=0; \
	total_failures=0; \
	total_ignored=0; \
	failed_bins=0; \
	for test_bin in $(TEST_BINS); do \
		printf 'Running %s\n' "$$test_bin"; \
		output=`./$$test_bin`; \
		status=$$?; \
		printf '%s\n' "$$output"; \
		summary_line=`printf '%s\n' "$$output" | awk '/Tests [0-9]+ Failures [0-9]+ Ignored/ { line = $$0 } END { print line }'`; \
		if [ -n "$$summary_line" ]; then \
			set -- $$summary_line; \
			total_tests=`expr $$total_tests + $$1`; \
			total_failures=`expr $$total_failures + $$3`; \
			total_ignored=`expr $$total_ignored + $$5`; \
		fi; \
		if [ $$status -ne 0 ]; then \
			failed_bins=`expr $$failed_bins + 1`; \
		fi; \
	done; \
	passed_tests=`expr $$total_tests - $$total_failures - $$total_ignored`; \
	printf '\nSummary: %s run, %s passed, %s failed, %s ignored\n' \
		"$$total_tests" "$$passed_tests" "$$total_failures" "$$total_ignored"; \
	test $$failed_bins -eq 0

stress: dirs $(STRESS_BINS)
	@total_tests=0; \
	total_failures=0; \
	total_ignored=0; \
	failed_bins=0; \
	for test_bin in $(STRESS_BINS); do \
		printf 'Running %s\n' "$$test_bin"; \
		output=`./$$test_bin`; \
		status=$$?; \
		printf '%s\n' "$$output"; \
		summary_line=`printf '%s\n' "$$output" | awk '/Tests [0-9]+ Failures [0-9]+ Ignored/ { line = $$0 } END { print line }'`; \
		if [ -n "$$summary_line" ]; then \
			set -- $$summary_line; \
			total_tests=`expr $$total_tests + $$1`; \
			total_failures=`expr $$total_failures + $$3`; \
			total_ignored=`expr $$total_ignored + $$5`; \
		fi; \
		if [ $$status -ne 0 ]; then \
			failed_bins=`expr $$failed_bins + 1`; \
		fi; \
	done; \
	passed_tests=`expr $$total_tests - $$total_failures - $$total_ignored`; \
	printf '\nSummary: %s run, %s passed, %s failed, %s ignored\n' \
		"$$total_tests" "$$passed_tests" "$$total_failures" "$$total_ignored"; \
	test $$failed_bins -eq 0

examples: dirs $(EXAMPLE_BINS)

# Core tests
$(BUILD_DIR)/test_core_%: tests/core/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS)

# Store tests
$(BUILD_DIR)/test_store_%: tests/store/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS)

# Container tests
$(BUILD_DIR)/test_containers_%: tests/containers/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS)

# Stress tests
$(BUILD_DIR)/stress_%: tests/stress/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS)

# Examples
$(BUILD_DIR)/%: examples/%.c $(SRC_SOURCES)
	$(CC) $(CFLAGS) $< $(SRC_SOURCES) -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

# ASAN variants - core
$(BUILD_DIR)/test_core_%-asan: tests/core/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(ASAN_CFLAGS)

# ASAN variants - store
$(BUILD_DIR)/test_store_%-asan: tests/store/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(ASAN_CFLAGS)

# ASAN variants - containers
$(BUILD_DIR)/test_containers_%-asan: tests/containers/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(ASAN_CFLAGS)

# ASAN variants - stress
$(BUILD_DIR)/stress_%-asan: tests/stress/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(ASAN_CFLAGS)

# UBSAN variants - core
$(BUILD_DIR)/test_core_%-ubsan: tests/core/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(UBSAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(UBSAN_CFLAGS)

# UBSAN variants - store
$(BUILD_DIR)/test_store_%-ubsan: tests/store/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(UBSAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(UBSAN_CFLAGS)

# UBSAN variants - containers
$(BUILD_DIR)/test_containers_%-ubsan: tests/containers/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(UBSAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(UBSAN_CFLAGS)

# UBSAN variants - stress
$(BUILD_DIR)/stress_%-ubsan: tests/stress/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(UBSAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(UBSAN_CFLAGS)

# Sanitize variants - core
$(BUILD_DIR)/test_core_%-sanitize: tests/core/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(SANITIZE_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(SANITIZE_LDFLAGS)

# Sanitize variants - store
$(BUILD_DIR)/test_store_%-sanitize: tests/store/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(SANITIZE_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(SANITIZE_LDFLAGS)

# Sanitize variants - containers
$(BUILD_DIR)/test_containers_%-sanitize: tests/containers/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(SANITIZE_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(SANITIZE_LDFLAGS)

# Sanitize variants - stress
$(BUILD_DIR)/stress_%-sanitize: tests/stress/%.c $(SRC_SOURCES) $(UNITY_SOURCES)
	$(CC) $(CFLAGS) $(SANITIZE_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(SANITIZE_LDFLAGS)

test-asan: dirs $(patsubst %,%-asan,$(TEST_BINS)) $(patsubst %,%-asan,$(STRESS_BINS))
	@echo "Run tests with ASAN enabled"

test-ubsan: dirs $(patsubst %,%-ubsan,$(TEST_BINS)) $(patsubst %,%-ubsan,$(STRESS_BINS))
	@echo "Run tests with UBSAN enabled"

test-sanitize: dirs $(patsubst %,%-sanitize,$(TEST_BINS)) $(patsubst %,%-sanitize,$(STRESS_BINS))
	@echo "Run tests with all sanitizers enabled"

stress-asan: dirs $(patsubst tests/stress/%.c,$(BUILD_DIR)/%-asan,$(TESTS_STRESS))
	@echo "Run stress tests with ASAN enabled"

stress-ubsan: dirs $(patsubst tests/stress/%.c,$(BUILD_DIR)/%-ubsan,$(TESTS_STRESS))
	@echo "Run stress tests with UBSAN enabled"

stress-sanitize: dirs $(patsubst tests/stress/%.c,$(BUILD_DIR)/%-sanitize,$(TESTS_STRESS))
	@echo "Run stress tests with all sanitizers enabled"
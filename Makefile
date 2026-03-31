CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Iinclude -Ithird_party/unity/src -pthread
LDFLAGS := -pthread
VERSION := 1.0.0

BUILD_DIR := build
SRC_SOURCES := $(wildcard src/*.c)
UNITY_SOURCES := third_party/unity/src/unity.c
STRESS_SOURCES := $(wildcard tests/*_stress.c)
TEST_SOURCES := $(filter-out $(STRESS_SOURCES),$(wildcard tests/*.c))
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SOURCES))
STRESS_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/%,$(STRESS_SOURCES))
EXAMPLE_SOURCES := $(wildcard examples/*.c)
EXAMPLE_BINS := $(patsubst examples/%.c,$(BUILD_DIR)/%,$(EXAMPLE_SOURCES))

SANITIZE_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
ASAN_CFLAGS := -fsanitize=address -fno-omit-frame-pointer -g
UBSAN_CFLAGS := -fsanitize=undefined -fno-omit-frame-pointer -g
SANITIZE_LDFLAGS := -fsanitize=address,undefined

.PHONY: all test stress examples clean dirs
.PHONY: test-asan test-ubsan test-sanitize
.PHONY: stress-asan stress-ubsan stress-sanitize
.PHONY: build-asan build-ubsan build-sanitize

all: dirs examples

dirs:
	mkdir -p $(BUILD_DIR)

test: dirs $(TEST_BINS)
	total_tests=0; \
	total_failures=0; \
	total_ignored=0; \
	failed_bins=0; \
	for test_bin in $(TEST_BINS); do \
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
	total_tests=0; \
	total_failures=0; \
	total_ignored=0; \
	failed_bins=0; \
	for test_bin in $(STRESS_BINS); do \
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

$(BUILD_DIR)/%: tests/%.c $(SRC_SOURCES)
	$(CC) $(CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%: examples/%.c $(SRC_SOURCES)
	$(CC) $(CFLAGS) $< $(SRC_SOURCES) -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/%-asan: tests/%.c $(SRC_SOURCES)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(ASAN_CFLAGS)

$(BUILD_DIR)/%-ubsan: tests/%.c $(SRC_SOURCES)
	$(CC) $(CFLAGS) $(UBSAN_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(UBSAN_CFLAGS)

$(BUILD_DIR)/%-sanitize: tests/%.c $(SRC_SOURCES)
	$(CC) $(CFLAGS) $(SANITIZE_CFLAGS) $< $(SRC_SOURCES) $(UNITY_SOURCES) -o $@ $(LDFLAGS) $(SANITIZE_LDFLAGS)

build-asan: dirs $(patsubst tests/%.c,$(BUILD_DIR)/%-asan,$(TEST_SOURCES) $(STRESS_SOURCES))
build-ubsan: dirs $(patsubst tests/%.c,$(BUILD_DIR)/%-ubsan,$(TEST_SOURCES) $(STRESS_SOURCES))
build-sanitize: dirs $(patsubst tests/%.c,$(BUILD_DIR)/%-sanitize,$(TEST_SOURCES) $(STRESS_SOURCES))

test-asan: dirs
	@failed_bins=0; \
	for test_bin in $(patsubst tests/%.c,$(BUILD_DIR)/%-asan,$(TEST_BINS)); do \
		output=`./$$test_bin 2>&1`; \
		status=$$?; \
		printf '%s\n' "$$output"; \
		summary_line=`printf '%s\n' "$$output" | awk '/Tests [0-9]+ Failures [0-9]+ Ignored/ { line = $$0 } END { print line }'`; \
		if [ $$status -ne 0 ] || echo "$$output" | grep -q "ERROR: AddressSanitizer"; then \
			failed_bins=`expr $$failed_bins + 1`; \
		fi; \
	done; \
	test $$failed_bins -eq 0

test-ubsan: dirs
	@failed_bins=0; \
	for test_bin in $(patsubst tests/%.c,$(BUILD_DIR)/%-ubsan,$(TEST_BINS)); do \
		output=`./$$test_bin 2>&1`; \
		status=$$?; \
		printf '%s\n' "$$output"; \
		summary_line=`printf '%s\n' "$$output" | awk '/Tests [0-9]+ Failures [0-9]+ Ignored/ { line = $$0 } END { print line }'`; \
		if [ $$status -ne 0 ] || echo "$$output" | grep -q "runtime error"; then \
			failed_bins=`expr $$failed_bins + 1`; \
		fi; \
	done; \
	test $$failed_bins -eq 0

test-sanitize: dirs
	@failed_bins=0; \
	for test_bin in $(patsubst tests/%.c,$(BUILD_DIR)/%-sanitize,$(TEST_BINS)); do \
		output=`./$$test_bin 2>&1`; \
		status=$$?; \
		printf '%s\n' "$$output"; \
		summary_line=`printf '%s\n' "$$output" | awk '/Tests [0-9]+ Failures [0-9]+ Ignored/ { line = $$0 } END { print line }'`; \
		if [ $$status -ne 0 ] || echo "$$output" | grep -qE "(AddressSanitizer|runtime error)"; then \
			failed_bins=`expr $$failed_bins + 1`; \
		fi; \
	done; \
	test $$failed_bins -eq 0

stress-asan: dirs $(patsubst tests/%.c,$(BUILD_DIR)/%-asan,$(STRESS_BINS))
	@failed_bins=0; \
	for test_bin in $(patsubst tests/%.c,$(BUILD_DIR)/%-asan,$(STRESS_BINS)); do \
		output=`./$$test_bin 2>&1`; \
		status=$$?; \
		printf '%s\n' "$$output"; \
		summary_line=`printf '%s\n' "$$output" | awk '/Tests [0-9]+ Failures [0-9]+ Ignored/ { line = $$0 } END { print line }'`; \
		if [ $$status -ne 0 ] || echo "$$output" | grep -q "ERROR: AddressSanitizer"; then \
			failed_bins=`expr $$failed_bins + 1`; \
		fi; \
	done; \
	test $$failed_bins -eq 0

stress-ubsan: dirs $(patsubst tests/%.c,$(BUILD_DIR)/%-ubsan,$(STRESS_BINS))
	@failed_bins=0; \
	for test_bin in $(patsubst tests/%.c,$(BUILD_DIR)/%-ubsan,$(STRESS_BINS)); do \
		output=`./$$test_bin 2>&1`; \
		status=$$?; \
		printf '%s\n' "$$output"; \
		summary_line=`printf '%s\n' "$$output" | awk '/Tests [0-9]+ Failures [0-9]+ Ignored/ { line = $$0 } END { print line }'`; \
		if [ $$status -ne 0 ] || echo "$$output" | grep -q "runtime error"; then \
			failed_bins=`expr $$failed_bins + 1`; \
		fi; \
	done; \
	test $$failed_bins -eq 0

stress-sanitize: dirs $(patsubst tests/%.c,$(BUILD_DIR)/%-sanitize,$(STRESS_BINS))
	@failed_bins=0; \
	for test_bin in $(patsubst tests/%.c,$(BUILD_DIR)/%-sanitize,$(STRESS_BINS)); do \
		output=`./$$test_bin 2>&1`; \
		status=$$?; \
		printf '%s\n' "$$output"; \
		summary_line=`printf '%s\n' "$$output" | awk '/Tests [0-9]+ Failures [0-9]+ Ignored/ { line = $$0 } END { print line }'`; \
		if [ $$status -ne 0 ] || echo "$$output" | grep -qE "(AddressSanitizer|runtime error)"; then \
			failed_bins=`expr $$failed_bins + 1`; \
		fi; \
	done; \
	test $$failed_bins -eq 0

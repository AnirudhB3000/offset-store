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

.PHONY: all test stress examples clean dirs

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

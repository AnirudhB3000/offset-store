CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Iinclude -pthread
LDFLAGS := -pthread
VERSION := 1.0.0

BUILD_DIR := build
SRC_SOURCES := $(wildcard src/*.c)
TEST_SOURCES := $(wildcard tests/*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SOURCES))
EXAMPLE_SOURCES := $(wildcard examples/*.c)
EXAMPLE_BINS := $(patsubst examples/%.c,$(BUILD_DIR)/%,$(EXAMPLE_SOURCES))

.PHONY: all test examples clean dirs

all: dirs examples

dirs:
	mkdir -p $(BUILD_DIR)

test: dirs $(TEST_BINS)
	for test_bin in $(TEST_BINS); do ./$$test_bin; done

examples: dirs $(EXAMPLE_BINS)

$(BUILD_DIR)/%: tests/%.c $(SRC_SOURCES)
	$(CC) $(CFLAGS) $< $(SRC_SOURCES) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%: examples/%.c $(SRC_SOURCES)
	$(CC) $(CFLAGS) $< $(SRC_SOURCES) -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

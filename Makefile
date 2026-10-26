CC      := gcc
CFLAGS  := -std=c2x -Wall -Wextra -Wpedantic -Wvla -Werror -g -Iinclude -MMD -MP
BUILD   := build
SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=$(BUILD)/%.o)
TARGET  := $(BUILD)/plantsense

LIB_OBJS  := $(filter-out $(BUILD)/main.o,$(OBJS))
TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.c $(LIB_OBJS) | $(BUILD)/tests
	$(CC) $(CFLAGS) $< $(LIB_OBJS) -o $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do $$t; done

$(BUILD) $(BUILD)/tests:
	mkdir -p $@

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(TEST_BINS:=.d)

.PHONY: all clean test

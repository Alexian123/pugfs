CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude
LDFLAGS =

BUILD_DIR = build
BIN = $(BUILD_DIR)/pugfs

SOURCES_C = $(shell find src -type f -name "*.c")
OBJECTS_C = $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(SOURCES_C))
HEADERS_C = $(shell find include src -type f -name "*.h" 2>/dev/null)
DEPS = $(OBJECTS_C:.o=.d)

.PHONY: all run clean

all: $(BIN)

$(BIN): $(OBJECTS_C)
	@mkdir -p $(@D)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "--> Built executable: $@"

$(BUILD_DIR)/obj/%.o: %.c $(HEADERS_C)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<
	@echo "--> Compiled: $<"

run: all
	./$(BIN)

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
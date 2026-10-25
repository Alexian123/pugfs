CC = gcc

CFLAGS = -Wall -Wextra -g -Iinclude -fPIC
LDFLAGS =

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
LIB_DIR = $(BUILD_DIR)/lib
BIN_DIR = $(BUILD_DIR)/bin

LIB_NAME = pugfs
LIB = $(LIB_DIR)/lib$(LIB_NAME).so
BIN = $(BIN_DIR)/main

# Library source files (including helpers)
SOURCES_C = $(shell find src -type f -name "*.c")

# Corresponding object files
OBJECTS_C = $(patsubst src/%.c, $(OBJ_DIR)/%.o, $(SOURCES_C))

# Public headers
HEADERS_C = $(shell find include -type f -name "*.h")

# Automatically generated dependency files
DEPS = $(OBJECTS_C:.o=.d) $(OBJ_DIR)/main.d

.PHONY: all run clean

all: $(LIB) $(BIN)

# Build the shared library
$(LIB): $(OBJECTS_C)
	@mkdir -p $(@D)
	$(CC) -shared -o $@ $^ $(LDFLAGS)
	@echo "--> Built library: $@"

# Compile library source files
$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<
	@echo "--> Compiled: $<"

# Compile the example program
$(OBJ_DIR)/main.o: examples/main.c $(HEADERS_C)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<
	@echo "--> Compiled: $<"

# Link the example against the shared library
$(BIN): $(OBJ_DIR)/main.o $(LIB)
	@mkdir -p $(@D)
	$(CC) -o $@ $(OBJ_DIR)/main.o \
		-L$(LIB_DIR) -l$(LIB_NAME) \
		-Wl,-rpath,'$$ORIGIN/../lib' \
		$(LDFLAGS)
	@echo "--> Linked executable: $@"

# Build and run the example
run: all
	./$(BIN)

# Remove generated files
clean:
	rm -rf $(BUILD_DIR)

# Include compiler-generated header dependencies
-include $(DEPS)
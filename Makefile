CC = gcc
CFLAGS = -Wall -Wextra -pthread -g -I./src
LDFLAGS = -pthread

# Directories
SRC_DIR = src
CLIENT_DIR = client
BUILD_DIR = build
STORAGE_DIR = storage

# Source files (only Task 1 for now)
SERVER_SRC = $(SRC_DIR)/server.c $(SRC_DIR)/client_queue.c
CLIENT_SRC = $(CLIENT_DIR)/client.c

# Object files
SERVER_OBJ = $(BUILD_DIR)/server.o $(BUILD_DIR)/client_queue.o
CLIENT_OBJ = $(BUILD_DIR)/client.o

# Executables
SERVER = server
CLIENT = test_client

.PHONY: all clean run valgrind tsan dirs

all: dirs $(SERVER) $(CLIENT)

dirs:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(STORAGE_DIR)

# Build server
$(SERVER): $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✓ Server built successfully"

# Build client
$(CLIENT): $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✓ Client built successfully"

# Compile server source files
$(BUILD_DIR)/server.o: $(SRC_DIR)/server.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/client_queue.o: $(SRC_DIR)/client_queue.c $(SRC_DIR)/client_queue.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile client source files
$(BUILD_DIR)/client.o: $(CLIENT_DIR)/client.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Run server
run: $(SERVER)
	./$(SERVER)

# Clean build files
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(SERVER) $(CLIENT)
	rm -rf $(STORAGE_DIR)
	@echo "✓ Cleaned build files"

# Test with valgrind
valgrind: $(SERVER)
	valgrind --leak-check=full --show-leak-kinds=all ./$(SERVER)

# Test with thread sanitizer (requires recompile)
tsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=thread" LDFLAGS="$(LDFLAGS) -fsanitize=thread"
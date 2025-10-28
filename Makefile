# Makefile for OS Project (Server & Client)

CC = gcc
CFLAGS = -Wall -pthread -g
TSAN_FLAGS = -fsanitize=thread -g -O1 -pthread

SERVER_SRC = server/server.c
CLIENT_SRC = client/client.c
SERVER_BIN = server/server
CLIENT_BIN = client/client
SERVER_TSAN_BIN = server/server_tsan

# Default target
all: $(SERVER_BIN) $(CLIENT_BIN)

# Build the server
$(SERVER_BIN): $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $(SERVER_BIN) $(SERVER_SRC)

# Build the client
$(CLIENT_BIN): $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $(CLIENT_BIN) $(CLIENT_SRC)

# Build server with ThreadSanitizer for race condition checks
tsan: $(SERVER_SRC)
	$(CC) $(TSAN_FLAGS) -o $(SERVER_TSAN_BIN) $(SERVER_SRC)

# Run Valgrind memory test on the server
valgrind:
	valgrind --leak-check=full --show-leak-kinds=all ./$(SERVER_BIN)

# Clean all compiled binaries and temporary files
clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN) $(SERVER_TSAN_BIN)
	rm -f *.o
	rm -rf client/client_folders/* server/client_folders/*

# Run the server
run-server:
	./$(SERVER_BIN)

# Run the client
run-client:
	./$(CLIENT_BIN)
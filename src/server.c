#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "client_queue.h"

#define DEFAULT_PORT 8080
#define BACKLOG 10

// Global variables
ClientQueue* client_queue = NULL;
int server_socket = -1;
volatile sig_atomic_t server_running = 1;

// Signal handler for graceful shutdown
void handle_signal(int sig) {
    (void)sig;  // Suppress unused parameter warning
    printf("\n[SIGNAL] Shutting down server...\n");
    server_running = 0;
    
    // Close server socket to unblock accept()
    if (server_socket >= 0) {
        close(server_socket);
    }
}

// Initialize server socket
int init_server(int port) {
    int sockfd;
    struct sockaddr_in server_addr;
    int opt = 1;
    
    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    // Set socket options (reuse address and port)
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        close(sockfd);
        return -1;
    }
    
    // Setup address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    // Bind socket
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        return -1;
    }
    
    // Listen for connections
    if (listen(sockfd, BACKLOG) < 0) {
        perror("Listen failed");
        close(sockfd);
        return -1;
    }
    
    printf("[SERVER] Listening on port %d\n", port);
    return sockfd;
}

// Accept connections and push to client queue
void accept_connections() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    printf("[SERVER] Ready to accept connections...\n");
    
    while (server_running) {
        int client_socket = accept(server_socket, 
                                   (struct sockaddr*)&client_addr, 
                                   &client_len);
        
        if (client_socket < 0) {
            if (server_running) {
                perror("Accept failed");
            }
            continue;
        }
        
        printf("[ACCEPT] New connection from %s:%d (socket fd: %d)\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port),
               client_socket);
        
        // Push socket to client queue
        client_queue_push(client_queue, client_socket);
    }
}

// Cleanup resources
void cleanup() {
    printf("\n[CLEANUP] Cleaning up resources...\n");
    
    if (client_queue) {
        client_queue_destroy(client_queue);
    }
    
    if (server_socket >= 0) {
        close(server_socket);
    }
    
    printf("[CLEANUP] Server shutdown complete.\n");
}

int main(int argc, char* argv[]) {
    int port = DEFAULT_PORT;
    
    printf("========================================\n");
    printf("  Dropbox Clone Server - Phase 1\n");
    printf("========================================\n");
    
    // Parse command line arguments
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Error: Invalid port number (must be 1-65535)\n");
            return EXIT_FAILURE;
        }
    }
    
    // Setup signal handlers for graceful shutdown
    signal(SIGINT, handle_signal);   // Ctrl+C
    signal(SIGTERM, handle_signal);  // kill command
    
    // Initialize client queue
    printf("[INIT] Initializing client queue...\n");
    client_queue = client_queue_init();
    if (!client_queue) {
        fprintf(stderr, "Error: Failed to initialize client queue\n");
        return EXIT_FAILURE;
    }
    
    // Initialize server socket
    printf("[INIT] Initializing server socket...\n");
    server_socket = init_server(port);
    if (server_socket < 0) {
        cleanup();
        return EXIT_FAILURE;
    }
    
    // TODO: Initialize client threadpool (Task 2)
    // TODO: Initialize task queue (Task 3)
    // TODO: Initialize worker threadpool (Task 3)
    
    printf("[INIT] Server initialized successfully\n");
    printf("[INFO] Press Ctrl+C to shutdown\n");
    printf("========================================\n\n");
    
    // Start accepting connections (main loop)
    accept_connections();
    
    // Cleanup on exit
    cleanup();
    
    return EXIT_SUCCESS;
}
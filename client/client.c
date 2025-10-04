#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 4096

int connect_to_server(const char* ip, int port) {
    int sockfd;
    struct sockaddr_in server_addr;
    
    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    // Setup server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sockfd);
        return -1;
    }
    
    // Connect to server
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sockfd);
        return -1;
    }
    
    printf("Connected to server at %s:%d\n", ip, port);
    return sockfd;
}

int main(int argc, char* argv[]) {
    const char* server_ip = "127.0.0.1";
    int port = DEFAULT_PORT;
    
    // Parse arguments
    if (argc > 1) {
        server_ip = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }
    
    // Connect to server
    int sockfd = connect_to_server(server_ip, port);
    if (sockfd < 0) {
        return EXIT_FAILURE;
    }
    
    // For now, just test connection and keep socket open
    printf("Connection established. Press Enter to disconnect...\n");
    getchar();
    
    // Close connection
    close(sockfd);
    printf("Disconnected from server.\n");
    
    return EXIT_SUCCESS;
}
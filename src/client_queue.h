#ifndef CLIENT_QUEUE_H
#define CLIENT_QUEUE_H

#include <pthread.h>

// Node for the client queue (linked list)
typedef struct ClientNode {
    int socket_fd;
    struct ClientNode* next;
} ClientNode;

// Thread-safe client queue
typedef struct {
    ClientNode* head;
    ClientNode* tail;
    int size;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    int shutdown;  // Flag for graceful shutdown
} ClientQueue;

// Function declarations
ClientQueue* client_queue_init();
void client_queue_push(ClientQueue* queue, int socket_fd);
int client_queue_pop(ClientQueue* queue);  // Blocks if empty
void client_queue_destroy(ClientQueue* queue);

#endif // CLIENT_QUEUE_H
#include "client_queue.h"
#include <stdlib.h>
#include <stdio.h>

// Initialize the client queue
ClientQueue* client_queue_init() {
    ClientQueue* queue = (ClientQueue*)malloc(sizeof(ClientQueue));
    if (!queue) {
        perror("Failed to allocate client queue");
        return NULL;
    }
    
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    queue->shutdown = 0;
    
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    
    printf("Client queue initialized\n");
    return queue;
}

// Push a socket to the queue (producer - main thread)
void client_queue_push(ClientQueue* queue, int socket_fd) {
    ClientNode* node = (ClientNode*)malloc(sizeof(ClientNode));
    if (!node) {
        perror("Failed to allocate client node");
        return;
    }
    
    node->socket_fd = socket_fd;
    node->next = NULL;
    
    pthread_mutex_lock(&queue->lock);
    
    if (queue->tail == NULL) {
        // Queue is empty
        queue->head = node;
        queue->tail = node;
    } else {
        // Add to tail
        queue->tail->next = node;
        queue->tail = node;
    }
    
    queue->size++;
    printf("Client queue: Added socket %d (queue size: %d)\n", socket_fd, queue->size);
    
    // Signal waiting threads
    pthread_cond_signal(&queue->not_empty);
    
    pthread_mutex_unlock(&queue->lock);
}

// Pop a socket from the queue (consumer - client threads)
// Blocks if queue is empty
int client_queue_pop(ClientQueue* queue) {
    pthread_mutex_lock(&queue->lock);
    
    // Wait while queue is empty and not shutting down
    while (queue->head == NULL && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->lock);
    }
    
    // Check if shutting down
    if (queue->shutdown && queue->head == NULL) {
        pthread_mutex_unlock(&queue->lock);
        return -1;  // Signal shutdown
    }
    
    // Pop from head
    ClientNode* node = queue->head;
    int socket_fd = node->socket_fd;
    
    queue->head = node->next;
    if (queue->head == NULL) {
        queue->tail = NULL;  // Queue is now empty
    }
    
    queue->size--;
    printf("Client queue: Removed socket %d (queue size: %d)\n", socket_fd, queue->size);
    
    pthread_mutex_unlock(&queue->lock);
    
    free(node);
    return socket_fd;
}

// Destroy the queue and free resources
void client_queue_destroy(ClientQueue* queue) {
    if (!queue) return;
    
    pthread_mutex_lock(&queue->lock);
    
    // Set shutdown flag and wake all waiting threads
    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);
    
    pthread_mutex_unlock(&queue->lock);
    
    // Free remaining nodes
    ClientNode* current = queue->head;
    while (current) {
        ClientNode* next = current->next;
        free(current);
        current = next;
    }
    
    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->not_empty);
    free(queue);
    printf("Client queue destroyed\n");
}
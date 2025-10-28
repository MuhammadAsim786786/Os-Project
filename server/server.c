// multi-threaded server.c
// Compile: gcc -o server server.c -pthread
// Run: ./server

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#include <errno.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define USERS_FILE "users.txt"
#define CLIENT_FOLDER "client_folders/"

pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;

/* === utility functions for networking === */
ssize_t send_all(int sock, const void *buf, size_t len) {
    size_t total = 0; const char *p = buf;
    while (total < len) {
        ssize_t s = send(sock, p + total, len - total, 0);
        if (s <= 0) return -1;
        total += s;
    }
    return total;
}

int send_line(int sock, const char *line) {
    if (send_all(sock, line, strlen(line)) < 0) return -1;
    return send_all(sock, "\n", 1);
}

ssize_t recv_line(int sock, char *buf, size_t maxlen) {
    size_t i = 0; char c;
    while (i + 1 < maxlen) {
        ssize_t r = recv(sock, &c, 1, 0);
        if (r == 0) return 0;
        if (r < 0) return -1;
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

ssize_t recv_nbytes(int sock, void *buf, size_t n) {
    size_t total = 0; char *p = buf;
    while (total < n) {
        ssize_t r = recv(sock, p + total, n - total, 0);
        if (r <= 0) return -1;
        total += r;
    }
    return (ssize_t)total;
}

/* === helper utilities === */
void trim_newline_local(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

/* === user management === */
int authenticate_user(const char *username, const char *password) {
    pthread_mutex_lock(&users_mutex);
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) { pthread_mutex_unlock(&users_mutex); return 0; }

    char line[512], u[128], p[128];
    int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_newline_local(line);
        if (sscanf(line, "%127[^:]:%127s", u, p) == 2)
            if (strcmp(u, username) == 0 && strcmp(p, password) == 0)
                ok = 1;
    }
    fclose(f);
    pthread_mutex_unlock(&users_mutex);
    return ok;
}

int register_user(const char *username, const char *password) {
    pthread_mutex_lock(&users_mutex);
    FILE *f = fopen(USERS_FILE, "a");
    if (!f) { pthread_mutex_unlock(&users_mutex); return -1; }
    fprintf(f, "%s:%s\n", username, password);
    fclose(f);
    pthread_mutex_unlock(&users_mutex);
    return 0;
}

int ensure_user_folder(const char *username) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s", CLIENT_FOLDER, username);
    pthread_mutex_lock(&files_mutex);
    struct stat st;
    if (stat(path, &st) == -1)
        mkdir(path, 0777);
    pthread_mutex_unlock(&files_mutex);
    return 0;
}

/* === file operations === */
void handle_upload(int client_sock, const char *username, const char *filename);
void handle_download(int client_sock, const char *username, const char *filename);
void handle_list(int client_sock, const char *username);
void handle_delete(int client_sock, const char *username, const char *filename);

/* === session handling === */
void handle_client_session(int client_sock) {
    char line[1024], username[128], password[128], choice[16];

    send_line(client_sock, "1. Sign Up");
    send_line(client_sock, "2. Login");
    send_line(client_sock, "Enter choice:");
    if (recv_line(client_sock, choice, sizeof(choice)) <= 0) { close(client_sock); return; }

    send_line(client_sock, "Enter username:");
    if (recv_line(client_sock, username, sizeof(username)) <= 0) { close(client_sock); return; }

    send_line(client_sock, "Enter password:");
    if (recv_line(client_sock, password, sizeof(password)) <= 0) { close(client_sock); return; }

    if (strcmp(choice, "1") == 0) {
        if (register_user(username, password) == 0) {
            ensure_user_folder(username);
            send_line(client_sock, "Signup successful");
        } else {
            send_line(client_sock, "ERROR: signup failed");
            close(client_sock); return;
        }
    } else {
        if (authenticate_user(username, password)) {
            ensure_user_folder(username);
            send_line(client_sock, "Login successful");
        } else {
            send_line(client_sock, "Login failed");
            close(client_sock); return;
        }
    }

    while (1) {
        send_line(client_sock, "Commands: UPLOAD <file>, DOWNLOAD <file>, LIST, DELETE <file>, QUIT");
        send_line(client_sock, "Enter command:");
        if (recv_line(client_sock, line, sizeof(line)) <= 0) break;

        if (strncmp(line, "UPLOAD ", 7) == 0) {
            char f[256]; sscanf(line + 7, "%s", f);
            handle_upload(client_sock, username, f);
        } else if (strncmp(line, "DOWNLOAD ", 9) == 0) {
            char f[256]; sscanf(line + 9, "%s", f);
            handle_download(client_sock, username, f);
        } else if (strcmp(line, "LIST") == 0) {
            handle_list(client_sock, username);
        } else if (strncmp(line, "DELETE ", 7) == 0) {
            char f[256]; sscanf(line + 7, "%s", f);
            handle_delete(client_sock, username, f);
        } else if (strcmp(line, "QUIT") == 0) {
            send_line(client_sock, "Goodbye");
            break;
        } else {
            send_line(client_sock, "ERROR: unknown command");
        }
    }

    close(client_sock);
    printf("[Server] Client %s disconnected\n", username);
}

/* === file handler implementations === */
void handle_upload(int client_sock, const char *username, const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s/%s", CLIENT_FOLDER, username, filename);
    send_line(client_sock, "READY");

    char sizebuf[64];
    if (recv_line(client_sock, sizebuf, sizeof(sizebuf)) <= 0) return;
    long size = atol(sizebuf);
    if (size <= 0) { send_line(client_sock, "ERROR: invalid size"); return; }

    pthread_mutex_lock(&files_mutex);
    FILE *fp = fopen(path, "wb");
    pthread_mutex_unlock(&files_mutex);
    if (!fp) { send_line(client_sock, "ERROR: cannot create file"); return; }

    char buffer[BUFFER_SIZE];
    long remaining = size;
    while (remaining > 0) {
        ssize_t n = recv(client_sock, buffer, (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining, 0);
        if (n <= 0) break;
        fwrite(buffer, 1, n, fp);
        remaining -= n;
    }
    fclose(fp);
    send_line(client_sock, "OK: file uploaded");
}

void handle_download(int client_sock, const char *username, const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s/%s", CLIENT_FOLDER, username, filename);

    pthread_mutex_lock(&files_mutex);
    FILE *fp = fopen(path, "rb");
    pthread_mutex_unlock(&files_mutex);
    if (!fp) { send_line(client_sock, "ERROR: file not found"); return; }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char msg[64];
    snprintf(msg, sizeof(msg), "SIZE %ld", size);
    send_line(client_sock, msg);

    char buf[BUFFER_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        send_all(client_sock, buf, n);
    fclose(fp);
    send_line(client_sock, "END_OF_FILE");
}

void handle_list(int client_sock, const char *username) {
    char folder[512];
    snprintf(folder, sizeof(folder), "%s%s", CLIENT_FOLDER, username);
    DIR *dir = opendir(folder);
    if (!dir) { send_line(client_sock, "ERROR: cannot open folder"); return; }

    send_line(client_sock, "BEGIN_LIST");
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] != '.')
            send_line(client_sock, e->d_name);
    }
    send_line(client_sock, "END_LIST");
    closedir(dir);
}

void handle_delete(int client_sock, const char *username, const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s/%s", CLIENT_FOLDER, username, filename);
    pthread_mutex_lock(&files_mutex);
    int res = unlink(path);
    pthread_mutex_unlock(&files_mutex);
    if (res == 0) send_line(client_sock, "OK: file deleted");
    else send_line(client_sock, "ERROR: cannot delete file");
}

/* === thread entry === */
void *client_thread(void *arg) {
    int client_sock = (intptr_t)arg;
    handle_client_session(client_sock);
    return NULL;
}

/* === main === */
int main() {
    mkdir(CLIENT_FOLDER, 0777);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);
    printf("Multi-threaded Server running on port %d...\n", PORT);

    while (1) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int client_sock = accept(server_fd, (struct sockaddr *)&client, &len);
        if (client_sock < 0) continue;
        printf("[Server] Connection from %s:%d\n",
               inet_ntoa(client.sin_addr), ntohs(client.sin_port));

        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, (void *)(intptr_t)client_sock);
        pthread_detach(tid);  // auto-clean threads
    }

    close(server_fd);
    return 0;
}


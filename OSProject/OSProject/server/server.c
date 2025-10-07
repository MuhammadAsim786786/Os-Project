// server.c
// Compile: gcc -o server server.c -pthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define USERS_FILE "users.txt"
#define CLIENT_FOLDER "client_folders/"

pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;

void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) {
        s[len-1] = '\0';
        len--;
    }
}

int send_all(int sock, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = buf;
    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

int send_line(int sock, const char *msg) {
    if (send_all(sock, msg, strlen(msg)) < 0) return -1;
    if (send_all(sock, "\n", 1) < 0) return -1;
    return 0;
}

ssize_t recv_line(int sock, char *buf, size_t size) {
    size_t i = 0;
    char c;
    while (i + 1 < size) {
        ssize_t n = recv(sock, &c, 1, 0);
        if (n <= 0) return n;
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

int authenticate_user(const char *user, const char *pass) {
    pthread_mutex_lock(&users_mutex);
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) { pthread_mutex_unlock(&users_mutex); return 0; }

    char line[256], u[100], p[100];
    int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (sscanf(line, "%[^:]:%s", u, p) == 2)
            if (strcmp(u, user) == 0 && strcmp(p, pass) == 0)
                ok = 1;
    }
    fclose(f);
    pthread_mutex_unlock(&users_mutex);
    return ok;
}

void register_user(const char *user, const char *pass) {
    pthread_mutex_lock(&users_mutex);
    FILE *f = fopen(USERS_FILE, "a");
    if (f) {
        fprintf(f, "%s:%s\n", user, pass);
        fclose(f);
    }
    pthread_mutex_unlock(&users_mutex);
}

void ensure_user_folder(const char *username) {
    char path[256];
    snprintf(path, sizeof(path), "%s%s", CLIENT_FOLDER, username);
    struct stat st = {0};
    if (stat(path, &st) == -1) mkdir(path, 0777);
}

void handle_upload(int sock, const char *user, const char *fname) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s/%s", CLIENT_FOLDER, user, fname);
    send_line(sock, "READY");

    char sizebuf[64];
    if (recv_line(sock, sizebuf, sizeof(sizebuf)) <= 0) return;
    long size = atol(sizebuf);
    if (size <= 0) { send_line(sock, "ERROR: invalid size"); return; }

    pthread_mutex_lock(&files_mutex);
    FILE *fp = fopen(path, "wb");
    pthread_mutex_unlock(&files_mutex);
    if (!fp) { send_line(sock, "ERROR: cannot create file"); return; }

    char buf[BUFFER_SIZE];
    long remaining = size;
    while (remaining > 0) {
        ssize_t n = recv(sock, buf, (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining, 0);
        if (n <= 0) break;
        fwrite(buf, 1, n, fp);
        remaining -= n;
    }
    fclose(fp);
    send_line(sock, "OK: file uploaded");
}

void handle_download(int sock, const char *user, const char *fname) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s/%s", CLIENT_FOLDER, user, fname);

    pthread_mutex_lock(&files_mutex);
    FILE *fp = fopen(path, "rb");
    pthread_mutex_unlock(&files_mutex);
    if (!fp) { send_line(sock, "ERROR: file not found"); return; }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char msg[64];
    snprintf(msg, sizeof(msg), "SIZE %ld", size);
    send_line(sock, msg);

    char buf[BUFFER_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        send(sock, buf, n, 0);
    fclose(fp);
    send_line(sock, "END_OF_FILE");
}

void handle_list(int sock, const char *user) {
    char folder[256];
    snprintf(folder, sizeof(folder), "%s%s", CLIENT_FOLDER, user);

    DIR *dir = opendir(folder);
    if (!dir) { send_line(sock, "ERROR: cannot open folder"); return; }

    send_line(sock, "BEGIN_LIST");
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.')
            send_line(sock, entry->d_name);
    }
    send_line(sock, "END_LIST");
    closedir(dir);
}

void handle_delete(int sock, const char *user, const char *fname) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s/%s", CLIENT_FOLDER, user, fname);
    pthread_mutex_lock(&files_mutex);
    int res = unlink(path);
    pthread_mutex_unlock(&files_mutex);

    if (res == 0) send_line(sock, "OK: file deleted");
    else send_line(sock, "ERROR: cannot delete file");
}

int main() {
    mkdir(CLIENT_FOLDER, 0777);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
    listen(sfd, 5);
    printf("Server running on port %d...\n", PORT);

    int cfd;
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    cfd = accept(sfd, (struct sockaddr *)&caddr, &clen);
    printf("Client connected.\n");

    char buf[BUFFER_SIZE], user[100], pass[100], choice[10];
    send_line(cfd, "1. Sign Up");
    send_line(cfd, "2. Login");
    send_line(cfd, "Enter choice:");
    recv_line(cfd, choice, sizeof(choice));

    send_line(cfd, "Enter username:");
    recv_line(cfd, user, sizeof(user));
    send_line(cfd, "Enter password:");
    recv_line(cfd, pass, sizeof(pass));

    if (strcmp(choice, "1") == 0) {
        register_user(user, pass);
        ensure_user_folder(user);
        send_line(cfd, "Signup successful!");
    } else if (strcmp(choice, "2") == 0) {
        if (authenticate_user(user, pass)) {
            ensure_user_folder(user);
            send_line(cfd, "Login successful!");
        } else {
            send_line(cfd, "Login failed!");
            close(cfd);
            close(sfd);
            return 0;
        }
    }

    while (1) {
        send_line(cfd, "Commands: UPLOAD <file>, DOWNLOAD <file>, LIST, DELETE <file>, QUIT");
        send_line(cfd, "Enter command:");
        recv_line(cfd, buf, sizeof(buf));

        if (strncmp(buf, "UPLOAD ", 7) == 0) {
            char f[100]; sscanf(buf + 7, "%s", f); handle_upload(cfd, user, f);
        } else if (strncmp(buf, "DOWNLOAD ", 9) == 0) {
            char f[100]; sscanf(buf + 9, "%s", f); handle_download(cfd, user, f);
        } else if (strcmp(buf, "LIST") == 0) {
            handle_list(cfd, user);
        } else if (strncmp(buf, "DELETE ", 7) == 0) {
            char f[100]; sscanf(buf + 7, "%s", f); handle_delete(cfd, user, f);
        } else if (strcmp(buf, "QUIT") == 0) {
            send_line(cfd, "Goodbye!");
            break;
        } else send_line(cfd, "Invalid command!");
    }

    close(cfd);
    close(sfd);
    return 0;
}


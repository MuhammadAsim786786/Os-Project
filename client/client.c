#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define CLIENT_FOLDER_BASE "client_folders/"

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

void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) {
        s[len-1] = '\0';
        len--;
    }
}

void build_local_path(char *dest, const char *username, const char *filename) {
    snprintf(dest, 512, "%s%s/%s", CLIENT_FOLDER_BASE, username, filename);
}

void ensure_local_user_folder(const char *username) {
    mkdir(CLIENT_FOLDER_BASE, 0777); // base folder
    char path[256];
    snprintf(path, sizeof(path), "%s%s", CLIENT_FOLDER_BASE, username);
    mkdir(path, 0777);
}

void do_upload(int sock, const char *username, const char *filename) {
    char localpath[512];
    build_local_path(localpath, username, filename);

    FILE *fp = fopen(localpath, "rb");
    if (!fp) { printf("Cannot open local file: %s\n", localpath); return; }

    char buf[BUFFER_SIZE];
    recv_line(sock, buf, sizeof(buf));
    if (strncmp(buf, "READY", 5) != 0) { printf("%s\n", buf); fclose(fp); return; }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char msg[64];
    snprintf(msg, sizeof(msg), "%ld", size);
    send_line(sock, msg);

    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        send_all(sock, buf, n);
    fclose(fp);

    recv_line(sock, buf, sizeof(buf));
    printf("%s\n", buf);
}

void do_download(int sock, const char *username, const char *filename) {
    char buf[BUFFER_SIZE];
    recv_line(sock, buf, sizeof(buf));
    if (strncmp(buf, "ERROR", 5) == 0) { printf("%s\n", buf); return; }

    long size;
    sscanf(buf, "SIZE %ld", &size);
    char localpath[512];
    build_local_path(localpath, username, filename);

    FILE *fp = fopen(localpath, "wb");
    if (!fp) { printf("Cannot create local file: %s\n", localpath); return; }

    long remaining = size;
    while (remaining > 0) {
        size_t chunk = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        if (recv_nbytes(sock, buf, chunk) != (ssize_t)chunk) break;
        fwrite(buf, 1, chunk, fp);
        remaining -= chunk;
    }
    fclose(fp);
    recv_line(sock, buf, sizeof(buf));
    printf("Downloaded to %s\n", localpath);
}

void do_list(int sock) {
    char buf[BUFFER_SIZE];
    recv_line(sock, buf, sizeof(buf));
    if (strcmp(buf, "BEGIN_LIST") != 0) { printf("%s\n", buf); return; }
    printf("Files:\n");
    while (1) {
        recv_line(sock, buf, sizeof(buf));
        if (strcmp(buf, "END_LIST") == 0) break;
        printf("- %s\n", buf);
    }
}

void do_delete(int sock) {
    char buf[BUFFER_SIZE];
    recv_line(sock, buf, sizeof(buf));
    printf("%s\n", buf);
}

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv.sin_addr);
    connect(sock, (struct sockaddr *)&serv, sizeof(serv));

    char buf[BUFFER_SIZE], cmd[256], username[128];

    // menu
    recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
    recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
    recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
    fgets(cmd, sizeof(cmd), stdin); trim_newline(cmd);
    send_line(sock, cmd);

    // username
    recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
    fgets(username, sizeof(username), stdin); trim_newline(username);
    send_line(sock, username);

    // password
    recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
    fgets(cmd, sizeof(cmd), stdin); trim_newline(cmd);
    send_line(sock, cmd);

    // result
    recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);

    ensure_local_user_folder(username); // ensure local folder exists

    while (1) {
        recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
        recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
        printf("> ");
        fgets(cmd, sizeof(cmd), stdin); trim_newline(cmd);
        send_line(sock, cmd);

        if (strncmp(cmd, "UPLOAD ", 7) == 0) {
            do_upload(sock, username, cmd + 7);
        } else if (strncmp(cmd, "DOWNLOAD ", 9) == 0) {
            do_download(sock, username, cmd + 9);
        } else if (strcmp(cmd, "LIST") == 0) {
            do_list(sock);
        } else if (strncmp(cmd, "DELETE ", 7) == 0) {
            do_delete(sock);
        } else if (strcmp(cmd, "QUIT") == 0) {
            recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
            break;
        } else {
            recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
        }
    }

    close(sock);
    return 0;
}


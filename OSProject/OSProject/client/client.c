// client.c
// Compile: gcc -o client client.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define PORT 8080
#define BUFFER_SIZE 4096

void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) {
        s[len-1] = '\0';
        len--;
    }
}

int send_line(int sock, const char *msg) {
    send(sock, msg, strlen(msg), 0);
    send(sock, "\n", 1, 0);
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

void upload_file(int sock, const char *fname) {
    FILE *fp = fopen(fname, "rb");
    if (!fp) { printf("Cannot open file: %s\n", fname); return; }

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
        send(sock, buf, n, 0);
    fclose(fp);

    recv_line(sock, buf, sizeof(buf));
    printf("%s\n", buf);
}

void download_file(int sock, const char *fname) {
    char buf[BUFFER_SIZE];
    recv_line(sock, buf, sizeof(buf));

    if (strncmp(buf, "ERROR", 5) == 0) { printf("%s\n", buf); return; }
    long size; sscanf(buf, "SIZE %ld", &size);

    FILE *fp = fopen(fname, "wb");
    if (!fp) { printf("Cannot create file\n"); return; }

    long rem = size;
    while (rem > 0) {
        ssize_t n = recv(sock, buf, (rem > BUFFER_SIZE) ? BUFFER_SIZE : rem, 0);
        if (n <= 0) break;
        fwrite(buf, 1, n, fp);
        rem -= n;
    }
    fclose(fp);
    recv_line(sock, buf, sizeof(buf));
    printf("Download complete: %s\n", fname);
}

void list_files(int sock) {
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

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv.sin_addr);
    connect(sock, (struct sockaddr *)&serv, sizeof(serv));

    char buf[BUFFER_SIZE], cmd[256];
    for (int i = 0; i < 3; i++) { recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf); }
    fgets(cmd, sizeof(cmd), stdin); trim_newline(cmd); send_line(sock, cmd);

    recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
    fgets(cmd, sizeof(cmd), stdin); trim_newline(cmd); send_line(sock, cmd);

    recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
    fgets(cmd, sizeof(cmd), stdin); trim_newline(cmd); send_line(sock, cmd);

    recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);

    while (1) {
        recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
        recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);

        printf("> ");
        fgets(cmd, sizeof(cmd), stdin);
        trim_newline(cmd);
        send_line(sock, cmd);

        if (strncmp(cmd, "UPLOAD ", 7) == 0) {
            upload_file(sock, cmd + 7);
        } else if (strncmp(cmd, "DOWNLOAD ", 9) == 0) {
            download_file(sock, cmd + 9);
        } else if (strcmp(cmd, "LIST") == 0) {
            list_files(sock);
        } else if (strncmp(cmd, "DELETE ", 7) == 0) {
            recv_line(sock, buf, sizeof(buf)); printf("%s\n", buf);
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


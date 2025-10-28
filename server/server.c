#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <inttypes.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define USERS_FILE "users.txt"
#define SERVER_CLIENT_FOLDER "client_folders/"
#define TMP_UPLOAD_DIR "tmp_uploads/"

#define CLIENT_THREADPOOL_SIZE 4
#define WORKER_THREADPOOL_SIZE 4

ssize_t send_all(int sock, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = buf;
    while (total < len) {
        ssize_t s = send(sock, p + total, len - total, 0);
        if (s <= 0) return -1;
        total += s;
    }
    return total;
}
int send_line_no_lock(int sock, const char *line) {
    if (send_all(sock, line, strlen(line)) < 0) return -1;
    if (send_all(sock, "\n", 1) < 0) return -1;
    return 0;
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
void trim_nl(char *s) {
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == '\n' || s[l-1] == '\r')) { s[l-1] = '\0'; l--; }
}

pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct client_info {
    int sock;
    pthread_mutex_t write_mutex;
    char username[128];
    int logged_in;
    struct client_info *next;
} client_info_t;
int client_send_line(client_info_t *c, const char *line) {
    pthread_mutex_lock(&c->write_mutex);
    int res = send_line_no_lock(c->sock, line);
    pthread_mutex_unlock(&c->write_mutex);
    return res;
}
ssize_t client_send_bytes(client_info_t *c, const void *buf, size_t len) {
    pthread_mutex_lock(&c->write_mutex);
    ssize_t r = send_all(c->sock, buf, len);
    pthread_mutex_unlock(&c->write_mutex);
    return r;
}

typedef struct client_queue {
    client_info_t *head, *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
} client_queue_t;
void client_queue_init(client_queue_t *q) { q->head = q->tail = NULL; pthread_mutex_init(&q->mutex,NULL); pthread_cond_init(&q->cond,NULL); q->count=0; }
void client_queue_push(client_queue_t *q, client_info_t *c) {
    c->next = NULL;
    pthread_mutex_lock(&q->mutex);
    if (q->tail) q->tail->next = c; else q->head = c;
    q->tail = c; q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}
client_info_t *client_queue_pop(client_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->head == NULL) pthread_cond_wait(&q->cond, &q->mutex);
    client_info_t *c = q->head; q->head = c->next;
    if (q->head == NULL) q->tail = NULL;
    q->count--; c->next = NULL;
    pthread_mutex_unlock(&q->mutex);
    return c;
}
client_queue_t client_queue;

typedef enum { TASK_UPLOAD_MOVE, TASK_DOWNLOAD_SEND, TASK_LIST_SEND, TASK_DELETE_FILE } task_type_t;
typedef struct task {
    task_type_t type;
    client_info_t *client; // not owned
    char username[128];
    char filename[512];
    char tmp_path[1024]; // for upload
    // completion signaling:
    pthread_mutex_t done_mutex;
    pthread_cond_t done_cond;
    int done; // 0 not done, 1 done
    struct task *next;
} task_t;

typedef struct task_queue {
    task_t *head, *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
} task_queue_t;
void task_queue_init(task_queue_t *q) { q->head=q->tail=NULL; pthread_mutex_init(&q->mutex,NULL); pthread_cond_init(&q->cond,NULL); q->count=0; }
void task_queue_push(task_queue_t *q, task_t *t) {
    t->next = NULL;
    pthread_mutex_lock(&q->mutex);
    if (q->tail) q->tail->next = t; else q->head = t;
    q->tail = t; q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}
task_t *task_queue_pop(task_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->head == NULL) pthread_cond_wait(&q->cond, &q->mutex);
    task_t *t = q->head; q->head = t->next;
    if (q->head == NULL) q->tail = NULL;
    q->count--; t->next = NULL;
    pthread_mutex_unlock(&q->mutex);
    return t;
}
task_queue_t task_queue;

int authenticate_user_file(const char *username, const char *password) {
    pthread_mutex_lock(&users_mutex);
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) { pthread_mutex_unlock(&users_mutex); return 0; }
    char line[512], u[128], p[128]; int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_nl(line);
        if (sscanf(line, "%127[^:]:%127s", u, p) == 2) if (strcmp(u, username)==0 && strcmp(p,password)==0) { ok = 1; break; }
    }
    fclose(f); pthread_mutex_unlock(&users_mutex); return ok;
}
int register_user_file(const char *username, const char *password) {
    pthread_mutex_lock(&users_mutex);
    FILE *f = fopen(USERS_FILE, "a");
    if (!f) { pthread_mutex_unlock(&users_mutex); return -1; }
    fprintf(f, "%s:%s\n", username, password); fclose(f); pthread_mutex_unlock(&users_mutex);
    return 0;
}
int ensure_server_user_folder(const char *username) {
    char path[1024]; snprintf(path, sizeof(path), SERVER_CLIENT_FOLDER "%s", username);
    pthread_mutex_lock(&files_mutex);
    struct stat st;
    if (stat(path, &st) == -1) { if (mkdir(path, 0777) != 0) { pthread_mutex_unlock(&files_mutex); return -1; } }
    pthread_mutex_unlock(&files_mutex);
    return 0;
}

void worker_handle_upload_move(task_t *task) {
    char dest[2048];
    snprintf(dest, sizeof(dest), SERVER_CLIENT_FOLDER "%s/%s", task->username, task->filename);
    pthread_mutex_lock(&files_mutex);
    if (rename(task->tmp_path, dest) != 0) { // fallback copy
        FILE *src = fopen(task->tmp_path, "rb");
        if (src) {
            FILE *dst = fopen(dest, "wb");
            if (dst) {
                char buf[BUFFER_SIZE]; size_t r;
                while ((r = fread(buf,1,sizeof(buf),src))>0) fwrite(buf,1,r,dst);
                fclose(dst);
            }
            fclose(src);
            unlink(task->tmp_path);
        }
    }
    pthread_mutex_unlock(&files_mutex);
    client_send_line(task->client, "OK: uploaded");
}
void worker_handle_delete(task_t *task) {
    char path[2048]; snprintf(path, sizeof(path), SERVER_CLIENT_FOLDER "%s/%s", task->username, task->filename);
    pthread_mutex_lock(&files_mutex);
    int res = unlink(path);
    pthread_mutex_unlock(&files_mutex);
    if (res == 0) client_send_line(task->client, "OK: deleted"); else client_send_line(task->client, "ERROR: cannot delete file");
}
void worker_handle_list(task_t *task) {
    char folder[1024]; snprintf(folder, sizeof(folder), SERVER_CLIENT_FOLDER "%s", task->username);
    pthread_mutex_lock(&files_mutex);
    DIR *d = opendir(folder);
    if (!d) { pthread_mutex_unlock(&files_mutex); client_send_line(task->client, "ERROR: cannot open folder"); return; }
    client_send_line(task->client, "BEGIN_LIST");
    struct dirent *e;
    while ((e = readdir(d)) != NULL) { if (e->d_name[0] == '.') continue; client_send_line(task->client, e->d_name); }
    closedir(d);
    pthread_mutex_unlock(&files_mutex);
    client_send_line(task->client, "END_LIST");
}
void worker_handle_download(task_t *task) {
    char path[2048]; snprintf(path, sizeof(path), SERVER_CLIENT_FOLDER "%s/%s", task->username, task->filename);
    pthread_mutex_lock(&files_mutex);
    FILE *fp = fopen(path, "rb");
    if (!fp) { pthread_mutex_unlock(&files_mutex); client_send_line(task->client, "ERROR: file not found"); return; }
    fseek(fp,0,SEEK_END); long size = ftell(fp); fseek(fp,0,SEEK_SET);
    char size_line[64]; snprintf(size_line, sizeof(size_line), "SIZE %ld", size);
    client_send_line(task->client, size_line);
    char buf[BUFFER_SIZE]; size_t r;
    while ((r = fread(buf,1,sizeof(buf),fp))>0) { client_send_bytes(task->client, buf, r); }
    fclose(fp); pthread_mutex_unlock(&files_mutex);
    client_send_line(task->client, "END_OF_FILE");
}

void *worker_thread_func(void *arg) {
    (void)arg;
    while (1) {
        task_t *task = task_queue_pop(&task_queue);
        if (!task) continue;
        switch (task->type) {
            case TASK_UPLOAD_MOVE: worker_handle_upload_move(task); break;
            case TASK_DELETE_FILE: worker_handle_delete(task); break;
            case TASK_LIST_SEND: worker_handle_list(task); break;
            case TASK_DOWNLOAD_SEND: worker_handle_download(task); break;
            default: client_send_line(task->client, "ERROR: unknown task"); break;
        }
        // signal completion to waiting client thread (do NOT free task here)
        pthread_mutex_lock(&task->done_mutex);
        task->done = 1;
        pthread_cond_signal(&task->done_cond);
        pthread_mutex_unlock(&task->done_mutex);
    }
    return NULL;
}

void ensure_tmp_dir() { struct stat st; if (stat(TMP_UPLOAD_DIR, &st) == -1) mkdir(TMP_UPLOAD_DIR, 0777); }
void generate_tmp_path(char *out, size_t outlen) { static int seq=0; pid_t pid=getpid(); int s = _sync_add_and_fetch(&seq,1); snprintf(out,outlen, TMP_UPLOAD_DIR "upload%d_%d.tmp", (int)pid, s); }

void handle_client_session_direct(client_info_t *client) {
    char buf[BUFFER_SIZE];
    // menu
    client_send_line(client, "1. Sign Up");
    client_send_line(client, "2. Login");
    client_send_line(client, "Enter choice:");
    if (recv_line(client->sock, buf, sizeof(buf)) <= 0) { close(client->sock); return; }
    trim_nl(buf); char choice[16]; strncpy(choice, buf, sizeof(choice));
    client_send_line(client, "Enter username:");
    if (recv_line(client->sock, buf, sizeof(buf)) <= 0) { close(client->sock); return; }
    trim_nl(buf); strncpy(client->username, buf, sizeof(client->username));
    client_send_line(client, "Enter password:");
    if (recv_line(client->sock, buf, sizeof(buf)) <= 0) { close(client->sock); return; }
    trim_nl(buf); char password[128]; strncpy(password, buf, sizeof(password));
    if (strcmp(choice,"1")==0) {
        if (register_user_file(client->username, password) == 0) { ensure_server_user_folder(client->username); client_send_line(client, "Signup successful"); client->logged_in=1; }
        else { client_send_line(client, "ERROR: signup failed"); close(client->sock); return; }
    } else {
        if (authenticate_user_file(client->username, password)) { ensure_server_user_folder(client->username); client_send_line(client, "Login successful"); client->logged_in=1; }
        else { client_send_line(client, "Login failed"); close(client->sock); return; }
    }

    while (1) {
        client_send_line(client, "Commands: UPLOAD <file>, DOWNLOAD <file>, LIST, DELETE <file>, QUIT");
        client_send_line(client, "Enter command:");
        ssize_t r = recv_line(client->sock, buf, sizeof(buf));
        if (r <= 0) break;
        trim_nl(buf);

        if (strncmp(buf, "UPLOAD ", 7) == 0) {
            char filename[512]; if (sscanf(buf+7, "%511s", filename) != 1) { client_send_line(client,"ERROR: invalid filename"); continue; }
            client_send_line(client, "READY");
            char sizebuf[64]; if (recv_line(client->sock, sizebuf, sizeof(sizebuf))<=0) break; trim_nl(sizebuf);
            unsigned long long size = strtoull(sizebuf, NULL, 10);
            ensure_tmp_dir();
            char tmp_path[1024]; generate_tmp_path(tmp_path, sizeof(tmp_path));
            FILE *tf = fopen(tmp_path,"wb");
            if (!tf) { client_send_line(client,"ERROR: cannot create temp file"); continue; }
            unsigned long long remaining = size; char b[BUFFER_SIZE]; // intentional typo prevention
        }
        // *** NOTE: the code above had an accidental scratch ; we'll continue with correct implementation below ***
        break;
    }
    // this is placeholder, actual control flow continues in refined version below
}


void handle_client_session(client_info_t *client) {
    char buf[BUFFER_SIZE];

    client_send_line(client, "1. Sign Up");
    client_send_line(client, "2. Login");
    client_send_line(client, "Enter choice:");
    if (recv_line(client->sock, buf, sizeof(buf)) <= 0) { close(client->sock); return; }
    trim_nl(buf);
    char choice[16]; strncpy(choice, buf, sizeof(choice));

    client_send_line(client, "Enter username:");
    if (recv_line(client->sock, buf, sizeof(buf)) <= 0) { close(client->sock); return; }
    trim_nl(buf);
    strncpy(client->username, buf, sizeof(client->username));
    client->username[sizeof(client->username)-1] = '\0';

    client_send_line(client, "Enter password:");
    if (recv_line(client->sock, buf, sizeof(buf)) <= 0) { close(client->sock); return; }
    trim_nl(buf);
    char password[128]; strncpy(password, buf, sizeof(password));

    if (strcmp(choice, "1") == 0) {
        if (register_user_file(client->username, password) == 0) { ensure_server_user_folder(client->username); client_send_line(client, "Signup successful"); client->logged_in = 1; }
        else { client_send_line(client, "ERROR: signup failed"); close(client->sock); return; }
    } else {
        if (authenticate_user_file(client->username, password)) { ensure_server_user_folder(client->username); client_send_line(client, "Login successful"); client->logged_in = 1; }
        else { client_send_line(client, "Login failed"); close(client->sock); return; }
    }

    // main loop
    while (1) {
        client_send_line(client, "Commands: UPLOAD <file>, DOWNLOAD <file>, LIST, DELETE <file>, QUIT");
        client_send_line(client, "Enter command:");
        ssize_t r = recv_line(client->sock, buf, sizeof(buf));
        if (r <= 0) break;
        trim_nl(buf);

        if (strncmp(buf, "UPLOAD ", 7) == 0) {
            char filename[512];
            if (sscanf(buf + 7, "%511s", filename) != 1) { client_send_line(client, "ERROR: invalid filename"); continue; }

            // receive file bytes into temp file
            client_send_line(client, "READY");
            char sizebuf[64];
            if (recv_line(client->sock, sizebuf, sizeof(sizebuf)) <= 0) break;
            trim_nl(sizebuf);
            unsigned long long size = strtoull(sizebuf, NULL, 10);

            ensure_tmp_dir();
            char tmp_path[1024]; generate_tmp_path(tmp_path, sizeof(tmp_path));
            FILE *tf = fopen(tmp_path, "wb");
            if (!tf) { client_send_line(client, "ERROR: cannot create temp file"); continue; }
            unsigned long long remaining = size;
            char buffer[BUFFER_SIZE];
            while (remaining > 0) {
                size_t toread = (remaining > sizeof(buffer)) ? sizeof(buffer) : (size_t)remaining;
                if (recv_nbytes(client->sock, buffer, toread) != (ssize_t)toread) {
                    fclose(tf); unlink(tmp_path); client_send_line(client, "ERROR: transfer failed"); goto client_disconnect;
                }
                fwrite(buffer, 1, toread, tf);
                remaining -= toread;
            }
            fclose(tf);

            // create task and wait for worker to move file
            task_t *t = calloc(1, sizeof(task_t));
            t->type = TASK_UPLOAD_MOVE;
            t->client = client;
            strncpy(t->username, client->username, sizeof(t->username));
            strncpy(t->filename, filename, sizeof(t->filename));
            strncpy(t->tmp_path, tmp_path, sizeof(t->tmp_path));
            pthread_mutex_init(&t->done_mutex, NULL);
            pthread_cond_init(&t->done_cond, NULL);
            t->done = 0;

            task_queue_push(&task_queue, t);

            // wait for worker to finish this task
            pthread_mutex_lock(&t->done_mutex);
            while (!t->done) pthread_cond_wait(&t->done_cond, &t->done_mutex);
            pthread_mutex_unlock(&t->done_mutex);

            // cleanup task
            pthread_cond_destroy(&t->done_cond);
            pthread_mutex_destroy(&t->done_mutex);
            free(t);
            // loop continues (worker already sent OK to client)
        }
        else if (strncmp(buf, "DOWNLOAD ", 9) == 0) {
            char filename[512];
            if (sscanf(buf + 9, "%511s", filename) != 1) { client_send_line(client, "ERROR: invalid filename"); continue; }
            task_t *t = calloc(1,sizeof(task_t));
            t->type = TASK_DOWNLOAD_SEND;
            t->client = client;
            strncpy(t->username, client->username, sizeof(t->username));
            strncpy(t->filename, filename, sizeof(t->filename));
            pthread_mutex_init(&t->done_mutex, NULL);
            pthread_cond_init(&t->done_cond, NULL);
            t->done = 0;
            task_queue_push(&task_queue, t);
            pthread_mutex_lock(&t->done_mutex);
            while (!t->done) pthread_cond_wait(&t->done_cond, &t->done_mutex);
            pthread_mutex_unlock(&t->done_mutex);
            pthread_cond_destroy(&t->done_cond);
            pthread_mutex_destroy(&t->done_mutex);
            free(t);
        }
        else if (strcmp(buf, "LIST") == 0) {
            task_t *t = calloc(1,sizeof(task_t));
            t->type = TASK_LIST_SEND;
            t->client = client;
            strncpy(t->username, client->username, sizeof(t->username));
            pthread_mutex_init(&t->done_mutex, NULL);
            pthread_cond_init(&t->done_cond, NULL);
            t->done = 0;
            task_queue_push(&task_queue, t);
            pthread_mutex_lock(&t->done_mutex);
            while (!t->done) pthread_cond_wait(&t->done_cond, &t->done_mutex);
            pthread_mutex_unlock(&t->done_mutex);
            pthread_cond_destroy(&t->done_cond);
            pthread_mutex_destroy(&t->done_mutex);
            free(t);
        }
        else if (strncmp(buf, "DELETE ", 7) == 0) {
            char filename[512];
            if (sscanf(buf + 7, "%511s", filename) != 1) { client_send_line(client, "ERROR: invalid filename"); continue; }
            task_t *t = calloc(1,sizeof(task_t));
            t->type = TASK_DELETE_FILE;
            t->client = client;
            strncpy(t->username, client->username, sizeof(t->username));
            strncpy(t->filename, filename, sizeof(t->filename));
            pthread_mutex_init(&t->done_mutex, NULL);
            pthread_cond_init(&t->done_cond, NULL);
            t->done = 0;
            task_queue_push(&task_queue, t);
            pthread_mutex_lock(&t->done_mutex);
            while (!t->done) pthread_cond_wait(&t->done_cond, &t->done_mutex);
            pthread_mutex_unlock(&t->done_mutex);
            pthread_cond_destroy(&t->done_cond);
            pthread_mutex_destroy(&t->done_mutex);
            free(t);
        }
        else if (strcmp(buf, "QUIT") == 0) {
            client_send_line(client, "Goodbye");
            break;
        } else {
            client_send_line(client, "ERROR: unknown command");
        }
    }

client_disconnect:
    close(client->sock);
    return;
}

void *client_thread_func(void *arg) {
    (void)arg;
    while (1) {
        client_info_t *client = client_queue_pop(&client_queue);
        if (!client) continue;
        handle_client_session(client);
        pthread_mutex_destroy(&client->write_mutex);
        free(client);
    }
    return NULL;
}

void *accept_thread_func(void *arg) {
    (void)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }
    int opt = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0}; addr.sin_family = AF_INET; addr.sin_port = htons(PORT); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(listen_fd, 128) < 0) { perror("listen"); exit(1); }
    printf("[server] listening on %d\n", PORT);
    while (1) {
        struct sockaddr_in cli; socklen_t len = sizeof(cli);
        int client_sock = accept(listen_fd, (struct sockaddr *)&cli, &len);
        if (client_sock < 0) { perror("accept"); continue; }
        client_info_t *c = calloc(1,sizeof(client_info_t));
        c->sock = client_sock; pthread_mutex_init(&c->write_mutex, NULL); c->logged_in = 0; c->username[0] = '\0';
        client_queue_push(&client_queue, c);
        printf("[server] connection from %s:%d\n", inet_ntoa(cli.sin_addr), ntohs(cli.sin_port));
    }
    close(listen_fd);
    return NULL;
}

int main() {
    mkdir(SERVER_CLIENT_FOLDER, 0777);
    mkdir(TMP_UPLOAD_DIR, 0777);
    client_queue_init(&client_queue);
    task_queue_init(&task_queue);
    pthread_t accept_thread;
    pthread_create(&accept_thread, NULL, accept_thread_func, NULL);
    pthread_t cthreads[CLIENT_THREADPOOL_SIZE];
    for (int i=0;i<CLIENT_THREADPOOL_SIZE;i++) pthread_create(&cthreads[i], NULL, client_thread_func, NULL);
    pthread_t wthreads[WORKER_THREADPOOL_SIZE];
    for (int i=0;i<WORKER_THREADPOOL_SIZE;i++) pthread_create(&wthreads[i], NULL, worker_thread_func, NULL);
    pthread_join(accept_thread, NULL);
    return 0;
}
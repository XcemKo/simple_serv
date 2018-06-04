#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ev.h>
#include <time.h>
#include <pthread.h>

#define CLIENT_BUF_IN_SIZE 4096
#define CLIENT_BUF_OUT_SIZE 4096

#define WORKERS_COUNT 4

struct ev_io_http {
    struct ev_io io;
    char *root_dir;
    unsigned int index;
    pthread_mutex_t *mutex;
};

struct pthread_ev {
    struct ev_loop *loop;
    struct ev_io *watcher;
    char buf_in[CLIENT_BUF_IN_SIZE];
};

extern int opterr;

void log_info(char *msg) {
    static FILE *f_log = NULL;
    if (f_log == NULL) {
        f_log = fopen("/home/box/info.log", "w");
        if (f_log == NULL) {
            perror("fopen serverl.log");
            exit(EXIT_FAILURE);
        }
    }

    time_t raw_cur_time;
    time(&raw_cur_time);
    struct tm *cur_time = localtime(&raw_cur_time);

    fprintf(
        f_log,
        "%04d-%02d-%02dT%02d:%02d:%02d ",
        1900 + cur_time->tm_year,
        cur_time->tm_mon,
        cur_time->tm_mday,
        cur_time->tm_hour,
        cur_time->tm_min,
        cur_time->tm_sec
    );
    fprintf(f_log, "\"");
    fprintf(f_log, msg);
    fprintf(f_log, "\"\n");
    fflush(f_log);
}

int parse_cli_args(int argc, char *const argv[], char **ip, int *port, char **dir) {
    int opt;
    opterr = 0;
    *ip = NULL;
    *dir = NULL;
    *port = 0;
    while ((opt = getopt(argc, argv, "h:p:d:")) != -1) {
        switch (opt) {
        case 'h':
            *ip = (char *)malloc(strlen(optarg) + 1);
            strcpy(*ip, optarg);
            break;

        case 'p':
            *port = atoi(optarg);
            break;
        case 'd':
            *dir = (char *)malloc(strlen(optarg) + 1);
            strcpy(*dir, optarg);
            break;
        }
    }
    if (!*ip || !*port || !*dir) {
        if (*ip) free(*ip);
        if (*dir) free(*dir);
        return -1;
    }
    return 0;
}

int start_socket(char *ip, int port) {
    int sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int optval = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton(ip, &addr.sin_addr) == 0)  {
        perror("inet_aton");
        exit(EXIT_FAILURE);
    }
    if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    if (listen(sd, SOMAXCONN) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return sd;
}

char *get_path_from_http_request(char *http_request) {
    size_t l = strlen(http_request);
    size_t b = 0, e = 0;
    while (b < l && http_request[b] != '/') b++;
    if (b == l) return NULL;
    e = b + 1;
    while (e < l && http_request[e] != ' ' && http_request[e] != '?') e++;
    if (e == l) return NULL;
    char *path = (char *)malloc(e - b + 1);
    memcpy(path, http_request + b, e - b);
    path[e-b] = '\0';
    return path;
}

void process_http_request(char *input, char *root_dir, char *output) {
    memset(output, '\0', CLIENT_BUF_OUT_SIZE);

    int status_code = 0;
    char *status_description;
    char *body = NULL;
    char headers[4096];
    memset(headers, '\0', 4096);
    strcat(headers, "Content-Type: text/html\r\n");

    char *path = get_path_from_http_request(input);
    if (path != NULL) {
        char *full_path = (char *)malloc(strlen(root_dir) + strlen(path) + sizeof("\0"));
        strcpy(full_path, root_dir);
        strcat(full_path, path);
        if (access(full_path, F_OK) == 0) {
            FILE *f = fopen(full_path, "r");
            if (f == NULL) {
                status_code = 403;
                status_description = "FORBIDDEN";
            } else {
                fseek(f, 0, SEEK_END);
                long f_size = ftell(f);
                fseek(f, 0, SEEK_SET);
                body = (char *)malloc(f_size + 1);
                fread(body, 1, f_size, f);
                body[f_size] = '\0';
                fclose(f);

                char cl_header[1024];
                sprintf(cl_header, "Content-Length: %d\r\n", f_size);
                strcat(headers, cl_header);

                status_code = 200;
                status_description = "OK";
            }
        } else {
            status_code = 404;
            status_description = "NOT FOUND";
        }
    } else {
        status_code = 400;
        status_description = "BAD REQUEST";
    }
    sprintf(
        output,
        "HTTP/1.0 %d %s\r\n%s\r\n%s",
        status_code,
        status_description,
        headers == NULL ? "" : headers,
        body == NULL ? "" : body
    );
}

void *read_client_handler(void *arg) {
    struct pthread_ev *pth_ev = (struct pthread_ev *)arg;

    struct ev_io_http *w = (struct ev_io_http *)pth_ev->watcher;

    char buf_out[CLIENT_BUF_OUT_SIZE];
    process_http_request(pth_ev->buf_in, w->root_dir, buf_out);

    // TODO: use buffered send
    send(w->io.fd, buf_out, strlen(buf_out) + 1, MSG_NOSIGNAL);

    shutdown(w->io.fd, SHUT_RDWR);
    close(w->io.fd);

    ev_io_stop(pth_ev->loop, &w->io);
    free(pth_ev);

    pthread_mutex_unlock(w->mutex);
    // printf("unlock: %d\n", w->index);
    free(w);
}

void read_client_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    struct ev_io_http *w = (struct ev_io_http *)watcher;

    char buf_in[CLIENT_BUF_IN_SIZE];
    // TODO: use buffered recv
    int read_len = recv(w->io.fd, &buf_in, CLIENT_BUF_IN_SIZE, MSG_NOSIGNAL);

    char log_msg[128];
    sprintf(log_msg, "recv: %d bytes", read_len);
    log_info(log_msg);

    if (read_len == -1) {
        perror("recv");
        exit(EXIT_FAILURE);
    } else if (read_len == 0) {
        ev_io_stop(loop, &w->io);
        return;
    }

    struct pthread_ev *pth_ev = (struct pthread_ev *)malloc(sizeof(struct pthread_ev));
    pth_ev->loop = loop;
    pth_ev->watcher = w;
    strcpy(pth_ev->buf_in, buf_in);

    pthread_mutex_lock(w->mutex);
    // printf("lock: %d\n", w->index);

    // read_client_handler((void *)pth_ev);

    pthread_t thread_handler;

    pthread_attr_t attr;
    // TODO: check returned value
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (
        pthread_create(
            &thread_handler,
            &attr,
            read_client_handler,
            (void *)pth_ev
        ) == -1
    ) {
        perror("pthred_create");
        exit(EXIT_FAILURE);
    }

    // TODO: check returned value
    pthread_attr_destroy(&attr);
}

void accept_client_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    static index = 0;

    static pthread_mutex_t mutexes[WORKERS_COUNT] = {
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER
    };

    struct ev_io_http *w = (struct ev_io_http *)watcher;

    int client_sd = accept(w->io.fd, 0, 0);

    struct ev_io_http *w_client = (struct ev_io_http *)malloc(sizeof(struct ev_io_http));
    w_client->root_dir = w->root_dir;
    w_client->index = index++ % WORKERS_COUNT;
    w_client->mutex = &mutexes[w_client->index];

    ev_io_init(&w_client->io, read_client_cb, client_sd, EV_READ);
    ev_io_start(loop, &w_client->io);
}

void start_server(char *ip, int port, char *root_dir) {
    int sd = start_socket(ip, port);

    struct ev_loop *loop = ev_default_loop(0);
    struct ev_io_http w_accept;
    w_accept.root_dir = root_dir;
    ev_io_init(&w_accept.io, accept_client_cb, sd, EV_READ);
    ev_io_start(loop, &w_accept.io);

    while (1) {
        ev_loop(loop, 0);
    }
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        printf("new pid server: %d\n", pid);
        exit(EXIT_SUCCESS);
    }
    if (setsid() == -1) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int main(int argc, char *const argv[]) {
    char *ip;
    int port = 0;
    char *dir;

    if (parse_cli_args(argc, argv, &ip, &port, &dir) != 0) {
        printf("Invalid args\n");
        exit(EXIT_FAILURE);
    }

    daemonize();

    char log_msg[256];
    sprintf(log_msg, "Server will be run with params: host %s; port %d; dir %s", ip, port, dir);
    log_info(log_msg);

    start_server(ip, port, dir);

    free(ip);
    free(dir);

    return 0;
}

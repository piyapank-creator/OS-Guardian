#include "httpd.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static char webdir_[256];
static char *(*json_cb_)(void);
static int lfd = -1;

static void reply(int fd, int code, const char *type, const char *body, size_t len) {
    char h[256];
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, code == 200 ? "OK" : "Not Found", type, len);
    send(fd, h, n, MSG_NOSIGNAL);
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(fd, body + off, len - off, MSG_NOSIGNAL);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void handle(int fd) {
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[2048];
    ssize_t n = recv(fd, req, sizeof req - 1, 0);
    if (n <= 0) return;
    req[n] = 0;
    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2 || strcmp(method, "GET")) {
        reply(fd, 404, "text/plain", "not found", 9); return;
    }
    if (!strcmp(path, "/api/state")) {
        char *j = json_cb_();
        reply(fd, 200, "application/json", j, strlen(j));
        free(j);
    } else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        char fp[300]; snprintf(fp, sizeof fp, "%s/index.html", webdir_);
        FILE *f = fopen(fp, "rb");
        if (!f) { reply(fd, 404, "text/plain", "index.html not found (use --web DIR)", 36); return; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
        char *b = malloc(sz + 1);
        size_t got = fread(b, 1, sz, f); fclose(f);
        reply(fd, 200, "text/html; charset=utf-8", b, got);
        free(b);
    } else {
        reply(fd, 404, "text/plain", "not found", 9);
    }
}

static void *loop(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) continue;
        handle(c);
        close(c);
    }
    return NULL;
}

int httpd_start(const char *host, int port, const char *webdir, char *(*cb)(void)) {
    snprintf(webdir_, sizeof webdir_, "%s", webdir);
    json_cb_ = cb;
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 16) < 0) return -1;
    pthread_t t;
    if (pthread_create(&t, NULL, loop, NULL)) return -1;
    pthread_detach(t);
    return 0;
}

#include "registry.h"
#include "guardian_client.h"
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

static void del(Claim *a, int *n, int pid, const char *res) {   /* res==NULL => all of pid */
    for (int i = 0; i < *n; ) {
        if (a[i].pid == pid && (!res || strcmp(a[i].res, res) == 0)) a[i] = a[--(*n)];
        else i++;
    }
}
static int has(Claim *a, int n, int pid, const char *res) {
    for (int i = 0; i < n; i++) if (a[i].pid == pid && !strcmp(a[i].res, res)) return 1;
    return 0;
}
static void add(Claim *a, int *n, int pid, const char *res) {
    if (*n >= MAX_EDGES || has(a, *n, pid, res)) return;
    a[*n].pid = pid; snprintf(a[*n].res, RES_LEN, "%s", res); (*n)++;
}

void registry_init(Registry *r) {
    memset(r, 0, sizeof *r);
    pthread_mutex_init(&r->mu, NULL);
}

void registry_apply(Registry *r, char cmd, int pid, const char *res) {
    if (pid <= 0) return;
    pthread_mutex_lock(&r->mu);
    switch (cmd) {
    case 'W': add(r->waits, &r->nw, pid, res); break;
    case 'H': del(r->waits, &r->nw, pid, res); add(r->holds, &r->nh, pid, res); break;
    case 'R': del(r->waits, &r->nw, pid, res); del(r->holds, &r->nh, pid, res); break;
    case 'X': del(r->waits, &r->nw, pid, NULL); del(r->holds, &r->nh, pid, NULL); break;
    }
    r->events++;
    pthread_mutex_unlock(&r->mu);
}

void registry_snapshot(Registry *r, Claim *h, Claim *w, int *nh, int *nw) {
    pthread_mutex_lock(&r->mu);
    memcpy(h, r->holds, r->nh * sizeof(Claim)); *nh = r->nh;
    memcpy(w, r->waits, r->nw * sizeof(Claim)); *nw = r->nw;
    pthread_mutex_unlock(&r->mu);
}

void registry_purge_dead(Registry *r) {
    pthread_mutex_lock(&r->mu);
    for (int i = 0; i < r->nh; ) {
        if (kill(r->holds[i].pid, 0) < 0 && errno == ESRCH) r->holds[i] = r->holds[--r->nh]; else i++;
    }
    for (int i = 0; i < r->nw; ) {
        if (kill(r->waits[i].pid, 0) < 0 && errno == ESRCH) r->waits[i] = r->waits[--r->nw]; else i++;
    }
    pthread_mutex_unlock(&r->mu);
}

/* ---- IPC server: one datagram = one event ("W 1234 R1") ---- */
static int sock_fd = -1;
typedef struct { Registry *r; sem_t *wake; } SrvArg;
static SrvArg srv;

static void *server_thread(void *arg) {
    (void)arg;
    char buf[128];
    for (;;) {
        ssize_t n = recv(sock_fd, buf, sizeof buf - 1, 0);
        if (n <= 0) { if (errno == EINTR) continue; break; }
        buf[n] = 0;
        char cmd; int pid; char res[RES_LEN] = "";
        if (sscanf(buf, "%c %d %31s", &cmd, &pid, res) >= 2) {
            registry_apply(srv.r, cmd, pid, res);
            sem_post(srv.wake);                 /* wake the detector immediately */
        }
    }
    return NULL;
}

int registry_server_start(Registry *r, sem_t *wake) {
    sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock_fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, GD_SOCK_PATH, sizeof a.sun_path - 1);
    unlink(GD_SOCK_PATH);
    if (bind(sock_fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    chmod(GD_SOCK_PATH, 0666);
    srv.r = r; srv.wake = wake;
    pthread_t t;
    if (pthread_create(&t, NULL, server_thread, NULL)) return -1;
    pthread_detach(t);
    return 0;
}
void registry_server_stop(void) { unlink(GD_SOCK_PATH); }

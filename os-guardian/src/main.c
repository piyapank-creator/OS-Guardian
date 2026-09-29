/* OS Guardian - Real-time Process Monitoring and Deadlock Detection
 *
 * Threads:
 *   monitor   : scans /proc every second               (Process/Resource Monitor)
 *   ipc server: receives hold/wait events on a socket   (IPC)  -> sem_post(wake)
 *   detector  : sem_timedwait(wake, 1s) -> WFG + DFS     (Deadlock Detector)
 *   http      : serves the dashboard + /api/state        (Dashboard)
 * All shared state lives in S and is protected by S.mu.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "deadlock.h"
#include "httpd.h"
#include "proc_monitor.h"
#include "registry.h"

#define MAX_PROCS 8192
#define HIST_N    20

typedef struct { char time[16]; char text[200]; } Event;

static struct {
    pthread_mutex_t mu;
    ProcInfo procs[MAX_PROCS]; int nprocs; SysInfo sys;
    Claim holds[MAX_EDGES], waits[MAX_EDGES]; int nh, nw;
    Report rep;
    char last_sig[256];
    Event hist[HIST_N]; int nhist;
} S = { .mu = PTHREAD_MUTEX_INITIALIZER };

static Registry REG;
static sem_t wake;

static void hhmmss(char *b, size_t n) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    strftime(b, n, "%H:%M:%S", &tm);
}

/* "P10 → P11 → P10 (via R1, R2)" ; long cycles are abbreviated */
static void cycle_text(const Cycle *c, char *out, size_t n) {
    size_t o = 0; int show = c->len < 8 ? c->len : 8;
    for (int i = 0; i < show && o < n; i++) o += snprintf(out + o, n - o, "P%d → ", c->pids[i]);
    if (show < c->len) o += snprintf(out + o, n - o, "… (%d processes) ", c->len);
    else if (o < n) o += snprintf(out + o, n - o, "P%d ", c->pids[0]);
    if (o < n) { o += snprintf(out + o, n - o, "(via "); }
    for (int i = 0; i < show && o < n; i++) o += snprintf(out + o, n - o, "%s%s", i ? ", " : "", c->res[i]);
    if (o < n) snprintf(out + o, n - o, "%s)", show < c->len ? ", …" : "");
}

static void push_event(const char *text) {          /* caller holds S.mu */
    memmove(&S.hist[1], &S.hist[0], sizeof(Event) * (HIST_N - 1));
    hhmmss(S.hist[0].time, sizeof S.hist[0].time);
    snprintf(S.hist[0].text, sizeof S.hist[0].text, "%s", text);
    if (S.nhist < HIST_N) S.nhist++;
    fprintf(stderr, "[%s] %s\n", S.hist[0].time, text);
}

/* ---------------- threads ---------------- */
static void *monitor_thread(void *arg) {
    (void)arg;
    static ProcInfo buf[MAX_PROCS];
    SysInfo sys;
    for (;;) {
        int n = proc_scan(buf, MAX_PROCS, &sys);
        pthread_mutex_lock(&S.mu);
        memcpy(S.procs, buf, n * sizeof(ProcInfo)); S.nprocs = n; S.sys = sys;
        pthread_mutex_unlock(&S.mu);
        sleep(1);
    }
    return NULL;
}

static void *detector_thread(void *arg) {
    (void)arg;
    static Claim h[MAX_EDGES], w[MAX_EDGES];
    static Report rep;
    for (;;) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
        sem_timedwait(&wake, &ts);                       /* event or 1s timeout */
        while (sem_trywait(&wake) == 0) ;                /* coalesce bursts */
        registry_purge_dead(&REG);
        int nh, nw;
        registry_snapshot(&REG, h, w, &nh, &nw);
        deadlock_detect(h, nh, w, nw, &rep);

        char sig[256] = "", text[200];
        for (int i = 0; i < rep.ncycles; i++) {
            const Cycle *c = &rep.cycles[i];
            int m = 0;                                    /* rotate so smallest pid first */
            for (int k = 1; k < c->len; k++) if (c->pids[k] < c->pids[m]) m = k;
            size_t o = strlen(sig);
            for (int k = 0; k < c->len && o < sizeof sig - 12; k++)
                o += snprintf(sig + o, sizeof sig - o, "%d,", c->pids[(m + k) % c->len]);
            if (o < sizeof sig - 2) strcat(sig, "|");
        }
        pthread_mutex_lock(&S.mu);
        memcpy(S.holds, h, nh * sizeof(Claim)); S.nh = nh;
        memcpy(S.waits, w, nw * sizeof(Claim)); S.nw = nw;
        memcpy(&S.rep, &rep, sizeof rep);
        if (strcmp(sig, S.last_sig)) {
            if (rep.ncycles) { char ct[160]; cycle_text(&rep.cycles[0], ct, sizeof ct);
                               snprintf(text, sizeof text, "Deadlock detected: %s", ct); push_event(text); }
            else               push_event("Deadlock resolved");
            strcpy(S.last_sig, sig);
        }
        pthread_mutex_unlock(&S.mu);
    }
    return NULL;
}

/* ---------------- JSON for the dashboard ---------------- */
typedef struct { char *p; size_t len, cap; } SB;
static void sb_add(SB *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < b->cap - b->len) { b->len += n; return; }
        b->cap = b->cap * 2 + (size_t)n + 64; b->p = realloc(b->p, b->cap);
    }
}
static void sb_str(SB *b, const char *s) {
    sb_add(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') sb_add(b, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) sb_add(b, " ");
        else sb_add(b, "%c", *s);
    }
    sb_add(b, "\"");
}
static int by_cpu(const void *a, const void *b) {
    const ProcInfo *x = a, *y = b;
    if (y->cpu != x->cpu) return y->cpu > x->cpu ? 1 : -1;
    return y->rss_kb > x->rss_kb ? 1 : (y->rss_kb < x->rss_kb ? -1 : 0);
}

static char *state_json(void) {
    SB b = { malloc(1 << 16), 0, 1 << 16 };
    static ProcInfo sorted[MAX_PROCS];      /* only the HTTP thread touches this */
    char tm[16]; hhmmss(tm, sizeof tm);
    pthread_mutex_lock(&S.mu);
    memcpy(sorted, S.procs, S.nprocs * sizeof(ProcInfo));
    int np = S.nprocs;
    qsort(sorted, np, sizeof(ProcInfo), by_cpu);

    sb_add(&b, "{\"time\":\"%s\",\"sys\":{\"cpu\":%.1f,\"mem_total_kb\":%ld,\"mem_avail_kb\":%ld,\"nproc\":%d,\"nthreads\":%d}",
           tm, S.sys.cpu_pct, S.sys.mem_total_kb, S.sys.mem_avail_kb, S.sys.nproc, S.sys.nthreads);

    sb_add(&b, ",\"procs\":[");
    for (int i = 0; i < np && i < 200; i++) {
        sb_add(&b, "%s{\"pid\":%d,\"name\":", i ? "," : "", sorted[i].pid);
        sb_str(&b, sorted[i].name);
        sb_add(&b, ",\"state\":\"%c\",\"cpu\":%.1f,\"rss_kb\":%ld,\"threads\":%d}",
               sorted[i].state, sorted[i].cpu, sorted[i].rss_kb, sorted[i].threads);
    }
    sb_add(&b, "]");

    sb_add(&b, ",\"holds\":[");
    for (int i = 0; i < S.nh; i++) { sb_add(&b, "%s{\"pid\":%d,\"res\":", i ? "," : "", S.holds[i].pid); sb_str(&b, S.holds[i].res); sb_add(&b, "}"); }
    sb_add(&b, "],\"waits\":[");
    for (int i = 0; i < S.nw; i++) { sb_add(&b, "%s{\"pid\":%d,\"res\":", i ? "," : "", S.waits[i].pid); sb_str(&b, S.waits[i].res); sb_add(&b, "}"); }
    sb_add(&b, "]");

    /* names for every pid that appears in the graph (may not be in the top-200) */
    sb_add(&b, ",\"names\":{");
    int first = 1;
    for (int pass = 0; pass < 2; pass++) {
        int cnt = pass ? S.nw : S.nh; const Claim *cl = pass ? S.waits : S.holds;
        for (int i = 0; i < cnt; i++) {
            int seen = 0;
            for (int j = 0; j < i && !seen; j++) seen = cl[j].pid == cl[i].pid;
            if (seen || (pass && 0)) continue;
            for (int k = 0; k < S.nprocs; k++) if (S.procs[k].pid == cl[i].pid) {
                sb_add(&b, "%s\"%d\":", first ? "" : ",", cl[i].pid); sb_str(&b, S.procs[k].name); first = 0; break;
            }
        }
    }
    sb_add(&b, "}");

    sb_add(&b, ",\"graph\":[");
    for (int i = 0; i < S.rep.nedges; i++) {
        sb_add(&b, "%s{\"from\":%d,\"to\":%d,\"res\":", i ? "," : "", S.rep.edges[i].from, S.rep.edges[i].to);
        sb_str(&b, S.rep.edges[i].res); sb_add(&b, "}");
    }
    sb_add(&b, "],\"deadlock\":{\"active\":%s,\"detect_us\":%.1f,\"cycles\":[", S.rep.ncycles ? "true" : "false", S.rep.elapsed_us);
    for (int i = 0; i < S.rep.ncycles; i++) {
        const Cycle *c = &S.rep.cycles[i];
        sb_add(&b, "%s{\"pids\":[", i ? "," : "");
        for (int k = 0; k < c->len; k++) sb_add(&b, "%s%d", k ? "," : "", c->pids[k]);
        sb_add(&b, "],\"res\":[");
        for (int k = 0; k < c->len; k++) { if (k) sb_add(&b, ","); sb_str(&b, c->res[k]); }
        sb_add(&b, "]}");
    }
    sb_add(&b, "]},\"history\":[");
    for (int i = 0; i < S.nhist; i++) {
        sb_add(&b, "%s{\"time\":\"%s\",\"text\":", i ? "," : "", S.hist[i].time);
        sb_str(&b, S.hist[i].text); sb_add(&b, "}");
    }
    sb_add(&b, "]}");
    pthread_mutex_unlock(&S.mu);
    return b.p;
}

/* ---------------- terminal UI ---------------- */
static void tui_loop(void) {
    static ProcInfo sorted[MAX_PROCS];
    for (;;) {
        sleep(1);
        pthread_mutex_lock(&S.mu);
        memcpy(sorted, S.procs, S.nprocs * sizeof(ProcInfo));
        int np = S.nprocs;
        qsort(sorted, np, sizeof(ProcInfo), by_cpu);
        printf("\033[H\033[J OS GUARDIAN   CPU %.0f%%   MEM %.0f%%   procs %d   threads %d\n\n",
               S.sys.cpu_pct, S.sys.mem_total_kb ? 100.0 * (S.sys.mem_total_kb - S.sys.mem_avail_kb) / S.sys.mem_total_kb : 0,
               S.sys.nproc, S.sys.nthreads);
        printf(" %-7s %-24s %-3s %6s %9s %4s\n", "PID", "NAME", "ST", "CPU%", "MEM(MB)", "THR");
        for (int i = 0; i < np && i < 15; i++)
            printf(" %-7d %-24.24s %-3c %6.1f %9.1f %4d\n", sorted[i].pid, sorted[i].name, sorted[i].state,
                   sorted[i].cpu, sorted[i].rss_kb / 1024.0, sorted[i].threads);
        printf("\n");
        if (S.rep.ncycles) {
            printf(" \033[1;31m*** DEADLOCK DETECTED ***\033[0m\n");
            for (int i = 0; i < S.rep.ncycles; i++) { char t[400]; cycle_text(&S.rep.cycles[i], t, sizeof t); printf("   %s\n", t); }
        } else printf(" \033[32m● NO DEADLOCK\033[0m   (%d waiting, %.0f µs)\n", S.nw, S.rep.elapsed_us);
        pthread_mutex_unlock(&S.mu);
        fflush(stdout);
    }
}

static void on_exit_sig(int s) { (void)s; registry_server_stop(); _exit(0); }

int main(int argc, char **argv) {
    const char *host = "127.0.0.1", *web = "web"; int port = 8080, tui = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--web") && i + 1 < argc) web = argv[++i];
        else if (!strcmp(argv[i], "--tui")) tui = 1;
        else { fprintf(stderr, "usage: %s [--port N] [--host IP] [--web DIR] [--tui]\n", argv[0]); return 1; }
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_exit_sig); signal(SIGTERM, on_exit_sig);

    sem_init(&wake, 0, 0);
    registry_init(&REG);
    if (registry_server_start(&REG, &wake)) { perror("ipc socket"); return 1; }
    pthread_t t1, t2;
    pthread_create(&t1, NULL, monitor_thread, NULL);
    pthread_create(&t2, NULL, detector_thread, NULL);
    if (httpd_start(host, port, web, state_json)) { perror("http"); return 1; }
    fprintf(stderr, "OS Guardian running: http://%s:%d  (IPC socket %s)\n", host, port, "/tmp/osguardian.sock");

    if (tui) tui_loop(); else for (;;) pause();
    return 0;
}

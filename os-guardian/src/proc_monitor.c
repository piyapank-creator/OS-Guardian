#include "proc_monitor.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HS 65536
typedef struct { int pid; unsigned long ticks; } Tick;
static Tick tab[2][HS];                 /* previous / current ticks by pid */
static int cur = 0;
static double last_t = 0;
static unsigned long long last_busy = 0, last_total = 0;

static double mono(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int prev_ticks(int pid, unsigned long *out) {
    Tick *p = tab[1 - cur];
    for (unsigned h = pid & (HS - 1); p[h].pid; h = (h + 1) & (HS - 1))
        if (p[h].pid == pid) { *out = p[h].ticks; return 1; }
    return 0;
}
static void put_ticks(int pid, unsigned long t) {
    Tick *p = tab[cur];
    unsigned h = pid & (HS - 1);
    while (p[h].pid) h = (h + 1) & (HS - 1);
    p[h].pid = pid; p[h].ticks = t;
}

int proc_scan(ProcInfo *out, int max, SysInfo *sys) {
    double now = mono();
    double dt = last_t > 0 ? now - last_t : 0;
    long hz = sysconf(_SC_CLK_TCK), pgkb = sysconf(_SC_PAGESIZE) / 1024;
    memset(tab[cur], 0, sizeof tab[cur]);

    int n = 0, nproc = 0, nthr = 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    char path[64], buf[2048];
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);

        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;                              /* process exited */
        size_t len = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[len] = 0;
        /* "pid (comm) S ..." - comm may contain spaces/parens, so use last ')' */
        char *lb = strchr(buf, '('), *rb = strrchr(buf, ')');
        if (!lb || !rb || rb < lb) continue;

        char state = '?'; unsigned long ut = 0, st = 0; long nt = 1;
        sscanf(rb + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %ld",
               &state, &ut, &st, &nt);
        unsigned long ticks = ut + st, prev;
        double cpu = 0;
        if (dt > 0 && prev_ticks(pid, &prev) && ticks >= prev)
            cpu = (double)(ticks - prev) / hz / dt * 100.0;
        put_ticks(pid, ticks);

        long rss_pages = 0;
        snprintf(path, sizeof path, "/proc/%d/statm", pid);
        if ((f = fopen(path, "r"))) { if (fscanf(f, "%*d %ld", &rss_pages) != 1) rss_pages = 0; fclose(f); }

        nproc++; nthr += (int)nt;
        if (n < max) {
            ProcInfo *p = &out[n++];
            size_t nl = (size_t)(rb - lb - 1);
            if (nl > sizeof p->name - 1) nl = sizeof p->name - 1;
            memcpy(p->name, lb + 1, nl); p->name[nl] = 0;
            p->pid = pid; p->state = state; p->cpu = cpu;
            p->rss_kb = rss_pages * pgkb; p->threads = (int)nt;
        }
    }
    closedir(d);

    /* system-wide CPU and memory */
    memset(sys, 0, sizeof *sys);
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long u, ni, s, id, io, irq, sirq, steal;
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &ni, &s, &id, &io, &irq, &sirq, &steal) == 8) {
            unsigned long long total = u + ni + s + id + io + irq + sirq + steal, busy = total - id - io;
            if (last_total && total > last_total) sys->cpu_pct = 100.0 * (busy - last_busy) / (total - last_total);
            last_busy = busy; last_total = total;
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %ld kB", &sys->mem_total_kb);
            sscanf(line, "MemAvailable: %ld kB", &sys->mem_avail_kb);
        }
        fclose(f);
    }
    sys->nproc = nproc; sys->nthreads = nthr;
    last_t = now; cur = 1 - cur;
    return n;
}

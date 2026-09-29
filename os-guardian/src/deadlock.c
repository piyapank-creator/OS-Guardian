#include "deadlock.h"
#include <string.h>
#include <time.h>

#define MAX_NODES (MAX_EDGES * 2)
#define HSIZE     16384                 /* power of two, > MAX_NODES */

typedef struct {
    int nn, pids[MAX_NODES];
    int hkey[HSIZE], hval[HSIZE];       /* pid -> node id (open addressing) */
    int head[MAX_NODES], color[MAX_NODES];
    int enext[MAX_EDGES], eto[MAX_EDGES];
    int path[MAX_NODES], pathedge[MAX_NODES];
    Report *rep;
} Ctx;

static Ctx C;

static int node_id(int pid) {
    unsigned h = ((unsigned)pid * 2654435761u) & (HSIZE - 1);
    while (C.hkey[h]) {
        if (C.hkey[h] == pid) return C.hval[h];
        h = (h + 1) & (HSIZE - 1);
    }
    int id = C.nn++;
    C.pids[id] = pid; C.hkey[h] = pid; C.hval[h] = id;
    C.head[id] = -1;  C.color[id] = 0;
    return id;
}

static void record_cycle(int v, int depth) {
    Report *r = C.rep;
    if (r->ncycles >= MAX_CYCLES) return;
    int k = depth;
    while (C.path[k] != v) k--;                     /* v is gray => on the path */
    int len = depth - k + 1;
    if (len > MAX_CYCLE_LEN) len = MAX_CYCLE_LEN;
    Cycle *cy = &r->cycles[r->ncycles++];
    cy->len = len;
    for (int i = 0; i < len; i++) {
        cy->pids[i] = C.pids[C.path[k + i]];
        strcpy(cy->res[i], r->edges[C.pathedge[k + i]].res);
    }
}

/* DFS with 3 colours: 0 = unvisited, 1 = on current path, 2 = finished.
 * An edge to a colour-1 node is a back edge => cycle => deadlock. */
static void dfs(int u, int depth) {
    C.color[u] = 1;
    C.path[depth] = u;
    for (int e = C.head[u]; e >= 0; e = C.enext[e]) {
        int v = C.eto[e];
        C.pathedge[depth] = e;
        if (C.color[v] == 0)      dfs(v, depth + 1);
        else if (C.color[v] == 1) record_cycle(v, depth);
    }
    C.color[u] = 2;
}

static double now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

int deadlock_detect(const Claim *holds, int nh, const Claim *waits, int nw, Report *out) {
    double t0 = now_us();
    memset(C.hkey, 0, sizeof C.hkey);
    C.nn = 0; C.rep = out;
    out->nedges = 0; out->ncycles = 0;

    /* 1. Resource Allocation Graph -> Wait-for Graph: P --res--> owner(res) */
    for (int i = 0; i < nw && out->nedges < MAX_EDGES; i++) {
        if (waits[i].pid <= 0) continue;
        for (int j = 0; j < nh; j++) {
            if (holds[j].pid == waits[i].pid || strcmp(holds[j].res, waits[i].res) != 0) continue;
            int a = node_id(waits[i].pid), b = node_id(holds[j].pid);
            int e = out->nedges++;
            out->edges[e].from = waits[i].pid;
            out->edges[e].to   = holds[j].pid;
            strcpy(out->edges[e].res, waits[i].res);
            C.eto[e] = b; C.enext[e] = C.head[a]; C.head[a] = e;
            break;
        }
    }
    /* 2. Cycle detection */
    for (int u = 0; u < C.nn; u++)
        if (C.color[u] == 0) dfs(u, 0);

    out->elapsed_us = now_us() - t0;
    return out->ncycles;
}

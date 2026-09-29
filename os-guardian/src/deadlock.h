/* deadlock.h - Wait-for Graph + DFS cycle detection */
#ifndef DEADLOCK_H
#define DEADLOCK_H

#define RES_LEN        32
#define MAX_EDGES      4096
#define MAX_CYCLES     4
#define MAX_CYCLE_LEN  1024

typedef struct { int pid; char res[RES_LEN]; } Claim;           /* "pid holds/waits res" */
typedef struct { int from, to; char res[RES_LEN]; } WfgEdge;    /* from waits for `to` via res */
typedef struct { int len; int pids[MAX_CYCLE_LEN]; char res[MAX_CYCLE_LEN][RES_LEN]; } Cycle;

typedef struct {
    int     nedges;  WfgEdge edges[MAX_EDGES];
    int     ncycles; Cycle   cycles[MAX_CYCLES];
    double  elapsed_us;                       /* time spent detecting */
} Report;

/* Build the Wait-for Graph from holds/waits and run DFS to find cycles.
 * Not thread-safe (uses static scratch space); call from one thread only.
 * Returns number of cycles found. */
int deadlock_detect(const Claim *holds, int nh, const Claim *waits, int nw, Report *out);

#endif

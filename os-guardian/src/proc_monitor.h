#ifndef PROC_MONITOR_H
#define PROC_MONITOR_H

typedef struct {
    int    pid;
    char   name[48];
    char   state;        /* R S D Z T ... */
    double cpu;          /* % of one core since previous scan */
    long   rss_kb;
    int    threads;
} ProcInfo;

typedef struct {
    double cpu_pct;      /* whole-system CPU busy % since previous scan */
    long   mem_total_kb, mem_avail_kb;
    int    nproc, nthreads;
} SysInfo;

/* Scan /proc. Fills up to `max` entries, returns how many were stored.
 * Keeps state between calls (for CPU deltas): call from ONE thread only. */
int proc_scan(ProcInfo *out, int max, SysInfo *sys);

#endif

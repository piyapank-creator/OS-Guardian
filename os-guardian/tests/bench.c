/* bench - Phase 6: performance evaluation of the detector and the /proc scan.
 * For N = 10/50/100/500/1000 synthetic processes it builds a ring of N processes
 * (a worst-case single big deadlock) and a chain (no deadlock), then reports
 * detection time, CPU cost at a 1 Hz check rate, and memory. Writes results.csv. */
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "deadlock.h"
#include "proc_monitor.h"

static Claim H[MAX_EDGES], W[MAX_EDGES];
static Report R;
static ProcInfo P[8192];

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

static double run(int n, int ring, int reps, int *cycles) {
    int nh = 0, nw = 0;
    for (int i = 0; i < n; i++) {                      /* process i holds Ri ... */
        H[nh].pid = 1000 + i; snprintf(H[nh++].res, RES_LEN, "R%d", i);
        if (ring || i < n - 1) {                        /* ... and waits for R(i+1) */
            W[nw].pid = 1000 + i; snprintf(W[nw++].res, RES_LEN, "R%d", (i + 1) % n);
        }
    }
    double t0 = now_us();
    for (int k = 0; k < reps; k++) *cycles = deadlock_detect(H, nh, W, nw, &R);
    return (now_us() - t0) / reps;
}

int main(void) {
    int sizes[] = { 10, 50, 100, 500, 1000 };
    FILE *csv = fopen("results.csv", "w");
    fprintf(csv, "processes,deadlock_us,no_deadlock_us,cpu_pct_at_1hz\n");
    printf("%-10s %-16s %-18s %-16s\n", "Processes", "Deadlock (us)", "No deadlock (us)", "CPU @1Hz (%)");
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        int n = sizes[s], c1, c2;
        double a = run(n, 1, 2000, &c1), b = run(n, 0, 2000, &c2);
        printf("%-10d %-16.2f %-18.2f %-16.5f   (cycles found: %d / %d)\n", n, a, b, a / 1e6 * 100, c1, c2);
        fprintf(csv, "%d,%.2f,%.2f,%.5f\n", n, a, b, a / 1e6 * 100);
    }
    fclose(csv);

    SysInfo sys; proc_scan(P, 8192, &sys);
    double t0 = now_us(); int n = 0;
    for (int i = 0; i < 20; i++) n = proc_scan(P, 8192, &sys);
    printf("\n/proc scan of %d real processes: %.0f us per scan\n", n, (now_us() - t0) / 20);
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    printf("peak memory of this benchmark: %.1f MB\nresults written to results.csv\n", ru.ru_maxrss / 1024.0);
    return 0;
}

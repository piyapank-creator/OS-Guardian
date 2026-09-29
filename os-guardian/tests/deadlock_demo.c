/* deadlock_demo - creates REAL deadlocks between processes using flock().
 *
 *   ./bin/deadlock_demo deadlock 3   # 3 processes in a ring -> real deadlock
 *   ./bin/deadlock_demo safe 3       # same locks, ordered acquisition -> no deadlock
 *
 * Each worker reports W(ait)/H(old)/R(elease) to OS Guardian before/after each lock.
 * Press Ctrl+C to stop and clean up.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include "guardian_client.h"
#define RES_NAME 32

static volatile sig_atomic_t stop = 0;
static void on_sig(int s) { (void)s; stop = 1; }

static void worker(int i, int n, int safe) {
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
    char nm[16]; snprintf(nm, sizeof nm, "osg_worker_%d", i); prctl(PR_SET_NAME, nm);
    int a = i, b = (i + 1) % n;
    if (safe && a > b) { int t = a; a = b; b = t; }      /* global lock order => no cycle */
    char ra[RES_NAME], rb[RES_NAME], pa[64], pb[64];
    snprintf(ra, sizeof ra, "R%d", a); snprintf(rb, sizeof rb, "R%d", b);
    snprintf(pa, sizeof pa, "/tmp/osg_%s", ra); snprintf(pb, sizeof pb, "/tmp/osg_%s", rb);
    for (;;) {
        int fa = open(pa, O_CREAT | O_RDWR, 0666), fb = open(pb, O_CREAT | O_RDWR, 0666);
        gd_report('W', ra); flock(fa, LOCK_EX); gd_report('H', ra);
        sleep(2);                                          /* everyone grabs the 1st lock */
        gd_report('W', rb); flock(fb, LOCK_EX); gd_report('H', rb);
        sleep(1);                                          /* "work" */
        flock(fb, LOCK_UN); gd_report('R', rb);
        flock(fa, LOCK_UN); gd_report('R', ra);
        close(fa); close(fb);
        usleep(300000);
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "deadlock") && strcmp(argv[1], "safe"))) {
        fprintf(stderr, "usage: %s deadlock|safe [N>=2]\n", argv[0]); return 1;
    }
    int safe = !strcmp(argv[1], "safe"), n = argc > 2 ? atoi(argv[2]) : 2;
    if (n < 2) n = 2;
    if (n > 200) n = 200;
    pid_t kids[200];
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) { worker(i, n, safe); _exit(0); }
        kids[i] = p;
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    printf("%s: %d workers started (pids %d..%d). Ctrl+C to stop.\n", argv[1], n, kids[0], kids[n - 1]);
    while (!stop) sleep(1);
    for (int i = 0; i < n; i++) kill(kids[i], SIGTERM);
    while (wait(NULL) > 0) ;
    for (int i = 0; i < n; i++) { char p[64]; snprintf(p, sizeof p, "/tmp/osg_R%d", i); unlink(p); }
    return 0;
}

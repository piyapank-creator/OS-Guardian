#ifndef REGISTRY_H
#define REGISTRY_H
#include <pthread.h>
#include <semaphore.h>
#include "deadlock.h"

/* Shared table of "who holds what / who waits for what", protected by a mutex. */
typedef struct {
    pthread_mutex_t mu;
    Claim holds[MAX_EDGES]; int nh;
    Claim waits[MAX_EDGES]; int nw;
    unsigned long events;
} Registry;

void registry_init(Registry *r);
void registry_apply(Registry *r, char cmd, int pid, const char *res);
void registry_snapshot(Registry *r, Claim *holds, Claim *waits, int *nh, int *nw);
void registry_purge_dead(Registry *r);                 /* drop claims of exited processes */
int  registry_server_start(Registry *r, sem_t *wake);  /* UNIX datagram socket thread */
void registry_server_stop(void);

#endif

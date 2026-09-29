/* guardian_client.h - tiny IPC client. Programs include this to tell
 * OS Guardian which resources they hold / wait for.
 *
 *   gd_report('W', "R1");  // about to wait for R1
 *   gd_report('H', "R1");  // now hold R1
 *   gd_report('R', "R1");  // released R1
 *   gd_report('X', "");    // process is done with everything
 */
#ifndef GUARDIAN_CLIENT_H
#define GUARDIAN_CLIENT_H
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define GD_SOCK_PATH "/tmp/osguardian.sock"

static inline void gd_report(char cmd, const char *res) {
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, GD_SOCK_PATH, sizeof a.sun_path - 1);
    char msg[96];
    int n = snprintf(msg, sizeof msg, "%c %d %s", cmd, (int)getpid(), res);
    sendto(s, msg, n, 0, (struct sockaddr *)&a, sizeof a);
    close(s);
}
#endif

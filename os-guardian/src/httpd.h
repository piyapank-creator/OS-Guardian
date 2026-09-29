#ifndef HTTPD_H
#define HTTPD_H
/* json_cb must return a malloc()'d string (freed by httpd). */
int httpd_start(const char *host, int port, const char *webdir, char *(*json_cb)(void));
#endif

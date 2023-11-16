#pragma once

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LISTENQ 8 /* second argument to listen() */
#define MAXLINE 256 /* max length of a line */
#define RIO_BUFSIZE 256
#define FILENAME_SIZE 64

typedef struct {
    char filename[FILENAME_SIZE];
    off_t offset; /* for support Range */
    size_t end;
    uint8_t servingGzip;

    uint8_t ws_key[24];
    int32_t ws_version;

    uint8_t non_local_hostname;
} http_request;

void log_access(int status, struct sockaddr_in* c_addr, http_request* req);
void client_error(int fd, int status, const char* msg, const char* longmsg);

ssize_t writen(int fd, const void* usrbuf, size_t n);

int rb_web_handle_websocket_switch_request(int fd, http_request* req);

void rb_web_set_wsprotocol(void* wsProtocolInstance);
void rb_web_clear_wsprotocol(void* wsProtocolInstance);
void rb_web_ws_new_connection(void* wsProtocolInstance, int fd);

#ifdef __cplusplus
};
#endif

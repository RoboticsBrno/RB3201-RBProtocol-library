#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/task.h"

#include "rbwebserver.h"
#include "rbwebserver_internal.h"
#include "rbdns.h"

#define TAG "RbWebServer"

#define EXTRA_DIRECTORY_SUFFIX "/extra/"

typedef struct {
    int rio_fd; /* descriptor for this buf */
    int rio_cnt; /* unread byte in this buf */
    char* rio_bufptr; /* next unread byte in this buf */
    char rio_buf[RIO_BUFSIZE]; /* internal buffer */
} rio_t;

/* Simplifies calls to bind(), connect(), and accept() */
typedef struct sockaddr SA;

typedef struct {
    const char* extension;
    const char* mime_type;
} mime_map;

static const mime_map meme_types[] = {
    { ".css", "text/css" },
    { ".html", "text/html" },
    { ".jpeg", "image/jpeg" },
    { ".jpg", "image/jpeg" },
    { ".ico", "image/x-icon" },
    { ".js", "application/javascript" },
    { ".png", "image/png" },
    { ".json", "application/json" },
    { NULL, NULL },
};

static const char* default_mime_type = "text/plain";

static void (*extra_path_callback)(const char *path, int out_fd) = NULL;
static not_found_response_t (*not_found_callback)(const char* request_path) = NULL;

static void *ws_protocol = NULL;
static const char *working_directory = "/notset";

void rb_web_set_extra_callback(void (*callback)(const char *path, int out_fd)) {
    extra_path_callback = callback;
}

void rb_web_set_not_found_callback(not_found_response_t (*callback)(const char* request_path)) {
    not_found_callback = callback;
}

void rb_web_set_wsprotocol(void *wsProtocolInstance) {
    if(ws_protocol != NULL && ws_protocol != wsProtocolInstance) {
        ESP_LOGE(TAG, "rb_web_set_wsprotocol was called twice with different instances!");
    }
    ws_protocol = wsProtocolInstance;
}

void rb_web_clear_wsprotocol(void *wsProtocolInstance) {
    if(ws_protocol == wsProtocolInstance) {
        ws_protocol = NULL;
    } else {
        ESP_LOGE(TAG, "rb_web_clear_wsprotocol was called twice with different instance than rb_web_set_wsprotocol!");
    }
}

static void rio_readinitb(rio_t* rp, int fd) {
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

ssize_t writen(int fd, const void* usrbuf, size_t n) {
    size_t nleft = n;
    ssize_t nwritten;
    const char* bufp = usrbuf;

    while (nleft > 0) {
        if ((nwritten = write(fd, bufp, nleft)) <= 0) {
            if (errno == EINTR) /* interrupted by sig handler return */
                nwritten = 0; /* and call write() again */
            else
                return -1; /* errorno set by write() */
        }
        nleft -= nwritten;
        bufp += nwritten;
    }
    return n;
}

/*
 * rio_read - This is a wrapper for the Unix read() function that
 *    transfers min(n, rio_cnt) bytes from an internal buffer to a user
 *    buffer, where n is the number of bytes requested by the user and
 *    rio_cnt is the number of unread bytes in the internal buffer. On
 *    entry, rio_read() refills the internal buffer via a call to
 *    read() if the internal buffer is empty.
 */
/* $begin rio_read */
static ssize_t rio_read(rio_t* rp, char* usrbuf, size_t n) {
    int cnt;
    while (rp->rio_cnt <= 0) { /* refill if buf is empty */

        rp->rio_cnt = recv(rp->rio_fd, rp->rio_buf,
            sizeof(rp->rio_buf), MSG_DONTWAIT);
        if (rp->rio_cnt < 0) {
            if(errno == EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else if (errno != EINTR) /* interrupted by sig handler return */
                return -1;
        } else if (rp->rio_cnt == 0) /* EOF */
            return 0;
        else
            rp->rio_bufptr = rp->rio_buf; /* reset buffer ptr */
    }

    /* Copy min(n, rp->rio_cnt) bytes from internal buf to user buf */
    cnt = n;
    if (rp->rio_cnt < n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

/*
 * rio_readlineb - robustly read a text line (buffered)
 */
static ssize_t rio_readlineb(rio_t* rp, void* usrbuf, size_t maxlen) {
    int n, rc;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++) {
        if ((rc = rio_read(rp, &c, 1)) == 1) {
            *bufp++ = c;
            if (c == '\n')
                break;
        } else if (rc == 0) {
            if (n == 1)
                return 0; /* EOF, no data read */
            else
                break; /* EOF, some data was read */
        } else
            return -1; /* error */
    }
    *bufp = 0;
    return n;
}

static const char* get_mime_type(char* filename) {
    char* dot = strrchr(filename, '.');
    if (dot) { // strrchar Locate last occurrence of character in string
        const mime_map* map = meme_types;
        while (map->extension) {
            if (strcmp(map->extension, dot) == 0) {
                return map->mime_type;
            }
            map++;
        }
    }
    return default_mime_type;
}

static int open_listenfd(int port) {
    int listenfd, optval = 1;
    struct sockaddr_in serveraddr;

    /* Create a socket descriptor */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    /* Eliminates "Address already in use" error from bind. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
            (const void*)&optval, sizeof(int))
        < 0)
        return -1;

    /* Listenfd will be an endpoint for all requests to port
       on any IP address for this host */
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)port);
    if (bind(listenfd, (SA*)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;
    return listenfd;
}

static void url_decode(char* src, char* dest, int max) {
    char* p = src;

    int prefix_len = snprintf(dest, FILENAME_SIZE, "%s/", working_directory);
    dest += prefix_len;
    max -= prefix_len;

    char code[3] = { 0 };
    while (*p && --max > 0) {
        if (*p == '%') {
            memcpy(code, ++p, 2);
            *dest++ = (char)strtoul(code, NULL, 16);
            p += 2;
        } else {
            *dest++ = *p++;
        }
    }
    *dest = '\0';
}

static int prepare_gzip(http_request* req) {
    int fd;

    if (req->servingGzip == 0)
        goto nogzip;

    const int fnlen = strlen(req->filename);
    if (fnlen < 3 || fnlen + 4 >= FILENAME_SIZE)
        goto nogzip;

    if (memcmp(req->filename + fnlen - 3, ".gz", 3) == 0)
        goto nogzip;

    memcpy(req->filename + fnlen, ".gz", 4);

    fd = open(req->filename, O_RDONLY);
    req->filename[fnlen] = 0;
    if (fd >= 0) {
        return fd;
    }

nogzip:
    req->servingGzip = 0;
    return open(req->filename, O_RDONLY);
}

static int is_local_host(const char *hostHeader) {
    const int hostHeaderLen = strlen(hostHeader) - 2; // ignore \r\n
    if(hostHeaderLen < 0) {
        return true;
    }

    if(hostHeaderLen >= 7 && hostHeaderLen <= 15) {
        int dots = 0;
        bool is_ip = true;
        for(int i = 0; i < hostHeaderLen; ++i) {
            char c = hostHeader[i];
            if(c == '.') {
                ++dots;
            } else if(c < '0' || c > '9') {
                is_ip = false;
                break;
            }
        }

        if(is_ip && dots == 3) {
            return true;
        }
    }

    const char *localHostname = rb_dn_get_local_hostname();
    if(strlen(localHostname) == hostHeaderLen && memcmp(localHostname, hostHeader, hostHeaderLen) == 0) {
        return true;
    }
    return false;
}

static void parse_request(int fd, http_request* req) {
    rio_t rio;
    char buf[MAXLINE], method[10], uri[MAXLINE];

    uint8_t websocket_upgrade_headers = 0;

    req->offset = 0;
    req->end = 0; /* default */
    req->servingGzip = 0;
    req->non_local_hostname = 0;

    rio_readinitb(&rio, fd);
    rio_readlineb(&rio, buf, MAXLINE);

    sscanf(buf, "%9s %255s", method, uri); /* version is not cared */
    /* read all */
    while (buf[0] != '\n' && buf[1] != '\n') { /* \n || \r\n */
        rio_readlineb(&rio, buf, MAXLINE);
        if (buf[0] == 'R' && buf[1] == 'a' && buf[2] == 'n') {
            sscanf(buf, "Range: bytes=%ld-%u", &req->offset, &req->end);
            // Range: [start, end]
            if (req->end != 0)
                req->end++;
        } else if (strstr(buf, "Accept-Encoding: ") == buf && strstr(buf + 17, "gzip")) {
            req->servingGzip = 1;
        } else if(strstr(buf, "Upgrade: websocket") == buf) {
            ++websocket_upgrade_headers;
        } else if(strstr(buf, "Connection: ") == buf && strstr(buf+12, "Upgrade")) {
            ++websocket_upgrade_headers;
        }
        //      len(Sec-WebSocket-Key: ) + len(key) + \r\n
        else if(strlen(buf) == 19 + 24 + 2 && strstr(buf, "Sec-WebSocket-Key: ") == buf) {
            ++websocket_upgrade_headers;
            memcpy(req->ws_key, buf + 19, 24);
        } else if(strstr(buf, "Sec-WebSocket-Version: ") == buf) {
            sscanf(buf + 23, "%ld", &req->ws_version);
        } else if(strncmp(buf, "Host: ", 6) == 0) {
            req->non_local_hostname = !is_local_host(buf+6);
        }
    }

    // Zero out the version if not all headers were received
    if(req->ws_version != 0 && (websocket_upgrade_headers != 3 || strcmp(method, "GET") != 0)) {
        req->ws_version = 0;
    }

    char* filename = uri;
    if (uri[0] == '/') {
        filename = uri + 1;

        if (filename[0] == 0) {
            strcpy(filename, "index.html");
        }

        int length = strlen(filename);
        if (length == 0) {
            filename = (char*)".";
        } else {
            for (int i = 0; i < length; ++i) {
                if (filename[i] == '?') {
                    filename[i] = '\0';
                    break;
                }
            }
        }
    }
    url_decode(filename, req->filename, FILENAME_SIZE);
}

void log_access(int status, struct sockaddr_in* c_addr, http_request* req) {
    ESP_LOGI(TAG, "%s:%d %d - %s", inet_ntoa(c_addr->sin_addr),
        ntohs(c_addr->sin_port), status, req->filename);
}

void client_error(int fd, int status, const char* msg, const char* longmsg) {
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.1 %d %s\r\n", status, msg);
    sprintf(buf + strlen(buf),
        "Content-length: %u\r\n\r\n", strlen(longmsg));
    sprintf(buf + strlen(buf), "%s", longmsg);
    writen(fd, buf, strlen(buf));
}

#define REDIRECT_RESPONSE \
    "HTTP/1.1 302 Temporary Redirect\r\n"\
    "Location: http://"

static void temporary_redirect(int fd, const char *location) {
    writen(fd, REDIRECT_RESPONSE, sizeof(REDIRECT_RESPONSE)-1);
    writen(fd, location, strlen(location));
    writen(fd, "\r\n\r\n", 4);
}


static ssize_t sendfile(char* buf, const size_t bufsize, int out_fd, int in_fd, off_t* offset, size_t count) {
    size_t chunk;
    ssize_t res;
    ssize_t chunk_res;

    if (*offset != 0 && lseek(in_fd, *offset, SEEK_SET) < 0) {
        ESP_LOGE(TAG, "Failed to seek for partial response: %d %s\n", errno, strerror(errno));
    }

    while (count > 0) {
        chunk = count;
        if (chunk > bufsize)
            chunk = bufsize;

        chunk_res = read(in_fd, buf, chunk);
        if (chunk_res < 0) {
            return -1;
        }

        chunk_res = write(out_fd, buf, chunk_res);
        if (chunk_res < 0) {
            return -1;
        }
        res += chunk_res;
        *offset += chunk_res;
        count -= chunk;
    }
    return res;
}

static void serve_headers_success(int out_fd, http_request* req, size_t total_size) {
    char buf[256];
    int buf_len = 0;
    if (req->offset > 0) {
        buf_len += snprintf(buf, sizeof(buf), "HTTP/1.1 206 Partial\r\nContent-Range: bytes %ld-%u/%u\r\n",
            req->offset, req->end, total_size);
    } else {
        buf_len += snprintf(buf, sizeof(buf), "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n");
    }

    if (req->servingGzip) {
        buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len - 1, "Content-Encoding: gzip\r\n");
    }

    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len - 1, "Content-Length: %lu\r\n", req->end - req->offset);

    if (strstr(req->filename, ".json")) {
        buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len - 1, "Cache-Control: no-store\r\n");
    } else {
        buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len - 1, "Cache-Control: private,max-age=259200\r\n");
    }

    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len - 1, "Content-Type: %s\r\n\r\n", get_mime_type(req->filename));

    writen(out_fd, buf, strlen(buf));
}

static void serve_static(int out_fd, int in_fd, http_request* req,
    size_t total_size) {

    serve_headers_success(out_fd, req, total_size);

    char buf[256];

    off_t offset = req->offset; /* copy */
    while (offset < req->end) {
        if (sendfile(buf, sizeof(buf), out_fd, in_fd, &offset, req->end - req->offset) <= 0) {
            break;
        }
        ESP_LOGD(TAG, "offset: %ld \n\n", offset);
        close(out_fd);
        break;
    }

}

static int serve_not_found_cb(int out_fd, http_request *req) {
    if(!not_found_callback) {
        return 0;
    }

    not_found_response_t nfr = not_found_callback(req->filename + strlen(working_directory));
    if(!nfr.data || !nfr.size) {
        return 0;
    }

    req->servingGzip = nfr.is_gzipped ? 1 : 0;
    if (req->end == 0 || req->end > nfr.size) {
        req->end = nfr.size;
    }

    serve_headers_success(out_fd, req, nfr.size);

    if (req->offset >= nfr.size) {
        return 1;
    }

    off_t pos = req->offset;
    while(pos < req->end) {
        size_t chunk = req->end - pos;
        if(chunk > 256) chunk = 256;
        writen(out_fd, nfr.data + pos, chunk);
        pos += chunk;
    }

    return 1;
}

static void process_serve_file(int fd, struct sockaddr_in* clientaddr, http_request *req) {
    struct stat sbuf;
    int status = 200;
    int ffd = prepare_gzip(req);
    if (ffd <= 0) {
        if(!serve_not_found_cb(fd, req)) {
            status = 404;
            client_error(fd, status, "Not found", "File not found");
        }
    } else {
        fstat(ffd, &sbuf);
        if (S_ISREG(sbuf.st_mode)) {
            if (req->end == 0) {
                req->end = sbuf.st_size;
            }
            if (req->offset > 0) {
                status = 206;
            }
            serve_static(fd, ffd, req, sbuf.st_size);
        } else {
            status = 400;
            client_error(fd, status, "Error", "Unknow Error");
        }
        close(ffd);
    }
    log_access(status, clientaddr, req);
}

static int process(int fd, struct sockaddr_in* clientaddr) {
    ESP_LOGD(TAG, "accept request, fd is %d\n", fd);
    http_request req;

    parse_request(fd, &req);

    char *extra_itr;

    if(req.non_local_hostname) {
        temporary_redirect(fd, rb_dn_get_local_hostname());
        return 0;
    } else if(req.ws_version != 0) {
        if(ws_protocol == NULL) {
            client_error(fd, 400, "WS not enabled", "");
            return 0;
        }

        if(rb_web_handle_websocket_switch_request(fd, &req) == 0) {
            return 1;
        }
        return 0;
    } else if((extra_itr = strstr(req.filename, EXTRA_DIRECTORY_SUFFIX)) != NULL &&
        (extra_itr - req.filename) == strlen(working_directory)) {
        if(extra_path_callback == NULL) {
            client_error(fd, 400, "Error", "No extra_path_callback specified.");
            return 0;
        }

        extra_path_callback(extra_itr + sizeof(EXTRA_DIRECTORY_SUFFIX)-1, fd);
        close(fd);
        return 0;
    } else {
        process_serve_file(fd, clientaddr, &req);
        return 0;
    }
}

static void tiny_web_task(void* portPtr) {
    int port = (int)portPtr;

    struct sockaddr_in clientaddr;
    int listenfd, connfd;
    socklen_t clientlen = sizeof clientaddr;
    TaskHandle_t stopping_task = NULL;

    listenfd = open_listenfd(port);
    if (listenfd > 0) {
        ESP_LOGI(TAG, "Listening on port %d", port);
    } else {
        ESP_LOGE(TAG, "failed to start: %s", strerror(errno));
        goto fail;
    }

    if (fcntl(listenfd, F_SETFL, O_NONBLOCK) == -1) {
        ESP_LOGE(TAG, "failed to set non-blocking: %s", strerror(errno));
        close(listenfd);
        goto fail;
    }

    while (xTaskNotifyWait(0, 0xFFFFFFFF, (uint32_t*)&stopping_task, pdMS_TO_TICKS(10)) == pdFALSE) {
        connfd = accept(listenfd, (SA*)&clientaddr, &clientlen);

        if (connfd >= 0) {
            // 5s timeout on writes
            struct timeval timeout;
            timeout.tv_sec = 5;
            timeout.tv_usec = 0;
            setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

            if(process(connfd, &clientaddr) <= 0) {
                close(connfd);
            } else {
                rb_web_ws_new_connection(ws_protocol, connfd);
            }
            continue;
        } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
            ESP_LOGE(TAG, "failed to accept: %s", strerror(errno));
        }
    }

    close(listenfd);
    goto exit;

fail:
    xTaskNotifyWait(0, 0xFFFFFFFF, (uint32_t*)&stopping_task, portMAX_DELAY);

exit:
    if(stopping_task)
        xTaskNotify(stopping_task, 0, eNoAction);
    vTaskDelete(NULL);
}

TaskHandle_t rb_web_start(int port) {
    if(!esp_spiffs_mounted(NULL)) {
        esp_vfs_spiffs_conf_t conf = {
            .base_path = "/data",
            .partition_label = NULL,
            .max_files = 5,
            .format_if_mount_failed = true
        };

        esp_err_t ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "Failed to mount or format filesystem");
            } else if (ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGE(TAG, "Failed to find SPIFFS partition");
            } else {
                ESP_LOGE(TAG, "Failed to initialize SPIFFS (%d)", ret);
            }
            return NULL;
        }
    }

    return rb_web_start_no_spiffs(port, "/data");
}

TaskHandle_t rb_web_start_no_spiffs(int port, const char *working_directory_path) {
    TaskHandle_t task;
    working_directory = working_directory_path;
    xTaskCreate(&tiny_web_task, "rbctrl_web", 3072, (void*)port, 2, &task);
    return task;
}

void rb_web_stop(TaskHandle_t web_task) {
    xTaskNotify(web_task, (uint32_t)xTaskGetCurrentTaskHandle(), eSetValueWithOverwrite);
    xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

    working_directory = "/notset";
    extra_path_callback = NULL;
    not_found_callback = NULL;
}

esp_err_t rb_web_add_file(const char* filename, const char* data, size_t len) {
    char buf[256];
    int fd;
    esp_err_t res = ESP_OK;

    snprintf(buf, sizeof(buf), "%s/%s", working_directory, filename);

    fd = open(buf, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return ESP_FAIL;
    }

    if (writen(fd, (void*)data, len) < 0) {
        res = ESP_FAIL;
    }

    close(fd);
    return res;
}

const char *rb_web_get_files_root(void) {
    return working_directory;
}

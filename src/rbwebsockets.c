#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "esp_log.h"
#include <string.h>

#include "rbwebserver.h"
#include "rbwebserver_internal.h"

#define TAG "RbWebServer"

#define KNOWN_WS_VERSION 13

#define WS_MAGIC_KEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define SWITCHING_RESP \
    "HTTP/1.1 101 Switching Protocols\r\n" \
    "Upgrade: websocket\r\n" \
    "Connection: Upgrade\r\n" \
    "Sec-WebSocket-Accept: "

int rb_web_handle_websocket_switch_request(int fd, http_request *req) {
    if(req->ws_version != KNOWN_WS_VERSION) {
        client_error(fd, 426, "Upgrade Required", "");
        return -1;
    }

    // Append magic key to key from browser
    uint8_t key_buf[24+sizeof(WS_MAGIC_KEY)-1];
    memcpy(key_buf, req->ws_key, 24);
    memcpy(key_buf+24, WS_MAGIC_KEY, sizeof(WS_MAGIC_KEY)-1); // minus \0 char

    // SHA1 the result
    uint8_t key_sha1[20];
    mbedtls_sha1(key_buf, sizeof(key_buf), key_sha1);

    // base64 encode the sha1
    size_t key_encoded_len = 0;
    mbedtls_base64_encode(key_buf, sizeof(key_buf), &key_encoded_len, key_sha1, sizeof(key_sha1));

    writen(fd, (void*)SWITCHING_RESP, sizeof(SWITCHING_RESP)-1);
    writen(fd, key_buf, key_encoded_len);
    writen(fd, (void*)"\r\n\r\n", 4);
    return 0;
}

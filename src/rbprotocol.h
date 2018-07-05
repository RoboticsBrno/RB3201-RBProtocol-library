#pragma once

#include "freertos/semphr.h"
#include <string>
#include <vector>

#include "rbjson.h"

#define RBPROTOCOL_PORT 42424

struct rb_string_view;

typedef void (*RbProtocolCallback)(void *cookie, const std::string& command, rbjson::Object *pkt);

class RbProtocol {
public:
    RbProtocol(const char *owner, const char *name, const char *description,
        RbProtocolCallback callback = NULL, void *callback_cookie = NULL);
    ~RbProtocol();

    void start(int port = RBPROTOCOL_PORT);
    void stop();

    void send(const char *cmd, rbjson::Object *params);
    void send_mustarrive(const char *cmd, rbjson::Object *params);

    void send_log(const char *fmt, ...);
    void send_log(const char *str);

private:
    struct MustArrive {
        rbjson::Object *pkt;
        uint32_t id;
        int16_t attempts;
    };

    static void read_task_trampoline(void *ctrl);
    void read_task();

    void send(struct sockaddr_in *addr, const char *command, rbjson::Object *obj);
    void send(struct sockaddr_in *addr, rbjson::Object *obj);
    void send(struct sockaddr_in *addr, const char *buf);
    void send(struct sockaddr_in *addr, const char *buf, size_t size);

    void handle_msg(struct sockaddr_in *addr, char *buf, ssize_t size);

    const char *m_owner;
    const char *m_name;
    const char *m_desc;

    RbProtocolCallback m_callback;
    void *m_callback_cookie;

    int m_socket;
    int m_read_counter;
    int m_write_counter;
    struct sockaddr_in m_possessed_addr;
    SemaphoreHandle_t m_mutex;

    uint32_t m_mustarrive_e;
    uint32_t m_mustarrive_f;
    std::vector<MustArrive> m_mustarrive_queue;
    SemaphoreHandle_t m_mustarrive_mutex;
};
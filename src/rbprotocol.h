#pragma once

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <functional>
#include <lwip/arch.h>
#include <lwip/sockets.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <stdarg.h>

#include "rbjson.h"

#define RBPROTOCOL_AXIS_MIN (-32767) //!< Minimal value of axes in "joy" command
#define RBPROTOCOL_AXIS_MAX (32767) //!< Maximal value of axes in "joy" command

namespace rb {

namespace internal {
enum ProtBackendType : uint8_t {
    PROT_NONE = 0,
    PROT_UDP = 1,
    PROT_WS = 2,
};

struct ProtocolAddrUdp {
    struct in_addr ip;
    uint16_t port;
};

struct ProtocolAddrWs {
    int fd;
};

struct ProtocolAddr {
    union {
        ProtocolAddrUdp udp;
        ProtocolAddrWs ws;
    };
    ProtBackendType kind;
};

struct QueueItem {
    ProtocolAddr addr;
    char* buf;
    uint16_t size;
};

class ProtBackendUdp;
class ProtBackendWs;
};

struct ProtocolConfig {
    bool enable_udp;
    bool enable_ws;
    bool ws_register_with_webserver;
    uint16_t udp_port;
};

class Protocol {
public:
    typedef std::function<void(const std::string& cmd, rbjson::Object* pkt)> callback_t;

    static const ProtocolConfig DEFAULT_CONFIG;

    Protocol(const char* owner, const char* name, const char* description, callback_t callback = nullptr);
    ~Protocol();

    esp_err_t start(const ProtocolConfig& cfg = DEFAULT_CONFIG);
    void stop();

    void send(const char* cmd, rbjson::Object* params = NULL);
    uint32_t send_mustarrive(const char* cmd, rbjson::Object* params = NULL);

    void send_log(const char* fmt, ...);
    void send_log(const char* fmt, va_list args);
    void send_log(const std::string& str);

    bool is_possessed() const; //!< Returns true of the device is possessed (somebody connected to it)
    bool is_mustarrive_complete(uint32_t id) const;

    TaskHandle_t getTaskSend() const { return m_task_send; }
    TaskHandle_t getTaskRecv() const { return m_task_recv; }

private:
    Protocol(Protocol&) = delete;

    struct MustArrive {
        rbjson::Object* pkt;
        uint32_t id;
        int16_t attempts;
    };

    static void send_task(void* selfVoid);
    static void recv_task(void* selfVoid);

    void handle_msg(const internal::ProtocolAddr& addr, rbjson::Object* pkt);
    void resend_mustarrive_locked();

    bool get_possessed_addr(internal::ProtocolAddr& addr) const;
    bool is_addr_empty(const internal::ProtocolAddr& addr) const;
    bool is_addr_same(const internal::ProtocolAddr& a, const internal::ProtocolAddr& b) const;

    void send(const internal::ProtocolAddr& addr, const char* command, rbjson::Object* obj);
    void send(const internal::ProtocolAddr& addr, rbjson::Object* obj);
    void send(const internal::ProtocolAddr& addr, const char* buf);
    void send(const internal::ProtocolAddr& addr, const char* buf, size_t size);

    const char* m_owner;
    const char* m_name;
    const char* m_desc;

    callback_t m_callback;

    int32_t m_read_counter;
    int32_t m_write_counter;
    internal::ProtocolAddr m_possessed_addr;
    QueueHandle_t m_sendQueue;
    internal::ProtBackendUdp* m_udp;
    internal::ProtBackendWs* m_ws;
    mutable std::mutex m_mutex;

    uint32_t m_mustarrive_e;
    uint32_t m_mustarrive_f;
    std::vector<MustArrive> m_mustarrive_queue;
    mutable std::mutex m_mustarrive_mutex;

    TaskHandle_t m_task_send;
    TaskHandle_t m_task_recv;
};

};

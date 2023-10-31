#include <errno.h>
#include <memory>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "rbjson.h"
#include "rbprotocoludp.h"

using namespace rbjson;

#define MS_TO_TICKS(ms) ((portTICK_PERIOD_MS <= ms) ? (ms / portTICK_PERIOD_MS) : 1)

#define MUST_ARRIVE_TIMER_PERIOD MS_TO_TICKS(100)
#define MUST_ARRIVE_ATTEMPTS 15

namespace rb {


ProtocolUdp::ProtocolUdp(const char* owner, const char* name, const char* description, ProtocolUdp::callback_t callback) :
    ProtocolImplBase(owner, name, description, callback) {

    m_socket = -1;

    memset(&m_possessed_addr, 0, sizeof(UdpSockAddr));
}


void ProtocolUdp::start(uint16_t port) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_task_send != nullptr) {
        return;
    }

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == -1) {
        ESP_LOGE(RBPROT_TAG, "failed to create socket: %s", strerror(errno));
        return;
    }

    int enable = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == -1) {
        ESP_LOGE(RBPROT_TAG, "failed to set SO_REUSEADDR: %s", strerror(errno));
        close(m_socket);
        m_socket = -1;
        return;
    }

    struct sockaddr_in addr_bind;
    memset(&addr_bind, 0, sizeof(addr_bind));
    addr_bind.sin_family = AF_INET;
    addr_bind.sin_port = htons(port);
    addr_bind.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_socket, (struct sockaddr*)&addr_bind, sizeof(addr_bind)) < 0) {
        ESP_LOGE(RBPROT_TAG, "failed to bind socket: %s", strerror(errno));
        close(m_socket);
        m_socket = -1;
        return;
    }

    xTaskCreate(&ProtocolUdp::send_task_trampoline, "rbctrl_send", 2560, this, 9, &m_task_send);
    xTaskCreate(&ProtocolUdp::recv_task_trampoline, "rbctrl_recv", 4096, this, 10, &m_task_recv);
}

void ProtocolUdp::stop() {
    ProtocolImplBase::stop();

    if (m_socket != -1) {
        close(m_socket);
        m_socket = -1;
    }
}

void ProtocolUdp::send_task_trampoline(void* ctrl) {
    ((ProtocolUdp*)ctrl)->send_task();
}

void ProtocolUdp::send_task() {
    TickType_t mustarrive_next;
    QueueItem it;
    struct sockaddr_in send_addr = {
        .sin_len = sizeof(struct sockaddr_in),
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr = { 0 },
        .sin_zero = { 0 },
    };

    m_mutex.lock();
    const int socket_fd = m_socket;
    m_mutex.unlock();

    mustarrive_next = xTaskGetTickCount() + MUST_ARRIVE_TIMER_PERIOD;

    while (true) {
        for (size_t i = 0; xQueueReceive(m_sendQueue, &it, MS_TO_TICKS(10)) == pdTRUE && i < 16; ++i) {
            if (it.buf == nullptr) {
                goto exit;
            }

            send_addr.sin_port = it.addr.port;
            send_addr.sin_addr = it.addr.ip;

            int res = ::sendto(socket_fd, it.buf, it.size, 0, (struct sockaddr*)&send_addr, sizeof(struct sockaddr_in));
            if (res < 0) {
                ESP_LOGE(RBPROT_TAG, "error in sendto: %d %s!", errno, strerror(errno));
            }
            delete[] it.buf;
        }

        if (xTaskGetTickCount() >= mustarrive_next) {
            m_mustarrive_mutex.lock();
            if (m_mustarrive_queue.size() != 0) {
                resend_mustarrive_locked();
            }
            m_mustarrive_mutex.unlock();
            mustarrive_next = xTaskGetTickCount() + MUST_ARRIVE_TIMER_PERIOD;
        }
    }

exit:
    vTaskDelete(nullptr);
}

void ProtocolUdp::resend_mustarrive_locked() {
    bool possesed;
    struct sockaddr_in send_addr = {
        .sin_len = sizeof(struct sockaddr_in),
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr = { 0 },
        .sin_zero = { 0 },
    };
    {
        UdpSockAddr addr;
        if((possesed = get_possessed_addr(addr))) {
            send_addr.sin_port = addr.port;
            send_addr.sin_addr = addr.ip;
        }
    }

    for (auto itr = m_mustarrive_queue.begin(); itr != m_mustarrive_queue.end();) {
        if (possesed) {
            m_mutex.lock();
            const int n = m_write_counter++;
            m_mutex.unlock();

            (*itr).pkt->set("n", n);

            const auto str = (*itr).pkt->str();

            int res = ::sendto(m_socket, str.c_str(), str.size(), 0, (struct sockaddr*)&send_addr, sizeof(struct sockaddr_in));
            if (res < 0) {
                ESP_LOGE(RBPROT_TAG, "error in sendto: %d %s!", errno, strerror(errno));
            }
        }

        if (++(*itr).attempts >= MUST_ARRIVE_ATTEMPTS) {
            delete (*itr).pkt;
            itr = m_mustarrive_queue.erase(itr);
        } else {
            ++itr;
        }
    }
}

void ProtocolUdp::recv_task_trampoline(void* ctrl) {
    ((ProtocolUdp*)ctrl)->recv_task();
}

void ProtocolUdp::recv_task() {
    m_mutex.lock();
    const int socket_fd = m_socket;
    m_mutex.unlock();

    struct sockaddr_in addr;
    socklen_t addr_len;
    size_t buf_size = 64;
    char* buf = (char*)malloc(buf_size);
    ssize_t res;

    while (true) {
        while (true) {
            res = recvfrom(socket_fd, buf, buf_size, MSG_PEEK, NULL, NULL);
            if (res < 0) {
                const auto err = errno;
                ESP_LOGE(RBPROT_TAG, "error in recvfrom: %d %s!", err, strerror(err));
                if (err == EBADF)
                    goto exit;
                vTaskDelay(MS_TO_TICKS(10));
                continue;
            }

            if (res < buf_size)
                break;
            buf_size += 16;
            buf = (char*)realloc(buf, buf_size);
        }

        addr_len = sizeof(struct sockaddr_in);
        const auto pop_res = recvfrom(socket_fd, buf, 0, 0, (struct sockaddr*)&addr, &addr_len);
        if (pop_res < 0) {
            const auto err = errno;
            ESP_LOGE(RBPROT_TAG, "error in recvfrom: %d %s!", err, strerror(err));
            if (err == EBADF)
                goto exit;
            vTaskDelay(MS_TO_TICKS(10));
            continue;
        }

        {
            std::unique_ptr<Object> pkt(parse(buf, res));
            if (!pkt) {
                ESP_LOGE(RBPROT_TAG, "failed to parse the packet's json");
            } else {
                UdpSockAddr sa = {
                    .ip = addr.sin_addr,
                    .port = addr.sin_port,
                };
                handle_msg(sa, pkt.get());
            }
        }
    }

exit:
    free(buf);
    vTaskDelete(nullptr);
}

}; // namespace rb

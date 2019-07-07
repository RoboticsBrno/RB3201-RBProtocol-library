#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <memory>
#include <stdarg.h>
#include <sys/time.h>

#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/udp.h"
#include <lwip/netdb.h>

#include "rbprotocol.h"
#include "rbjson.h"

using namespace rbjson;

static const char* TAG = "RbProtocol";

#define MUST_ARRIVE_TIMER_PERIOD 50
#define MUST_ARRIVE_ATTEMPTS 40

static int diff_ms(timeval& t1, timeval& t2) {
    return (((t1.tv_sec - t2.tv_sec) * 1000000) +
            (t1.tv_usec - t2.tv_usec))/1000;
}

namespace rb {

Protocol::Protocol(const char *owner, const char *name, const char *description, Protocol::callback_t callback) {
    m_owner = owner;
    m_name = name;
    m_desc = description;
    m_callback = callback;

    m_task_send = nullptr;
    m_task_recv = nullptr;

    m_possessed_port = 0;

    m_sendQueue = xQueueCreate(32, sizeof(SendQueueItem));
    m_recvQueue = xQueueCreate(32, sizeof(RecvQueueItem));

    m_read_counter = 0;
    m_write_counter = 0;

    m_mustarrive_e = 0;
    m_mustarrive_f = 0;

    memset(&m_possessed_addr, 0, sizeof(struct sockaddr_in));
}

Protocol::~Protocol() {
    stop();
}

void Protocol::start(u16_t port) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_task_send != nullptr) {
        return;
    }

    m_port = port;
    xTaskCreate(&Protocol::send_task_trampoline, "rbctrl_send", 3072, this, 5, &m_task_send);
    xTaskCreate(&Protocol::recv_task_trampoline, "rbctrl_recv", 4096, this, 5, &m_task_recv);
}

void Protocol::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_task_send == nullptr) {
        return;
    }

    SendQueueItem sit = { 0 };
    xQueueSend(m_sendQueue, &sit, portMAX_DELAY);

    RecvQueueItem rit = { 0 };
    xQueueSend(m_recvQueue, &rit, portMAX_DELAY);

    m_task_send = nullptr;
    m_task_recv = nullptr;
}

bool Protocol::is_possessed() const {
    m_mutex.lock();
    bool res = m_possessed_port != 0;
    m_mutex.unlock();
    return res;
}

bool Protocol::get_possessed_addr(ip_addr_t *addr, u16_t *port) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if(m_possessed_port == 0)
        return false;
    ip_addr_copy(*addr, m_possessed_addr);
    *port = m_possessed_port;
    return true;
}

void Protocol::send_mustarrive(const char *cmd, Object *params) {
    if(params == NULL) {
        params = new Object();
    }

    struct ip_addr addr;
    u16_t port;
    if(!get_possessed_addr(&addr, &port)) {
        ESP_LOGW(TAG, "can't send, the device was not possessed yet.");
        return;
    }

    MustArrive mr;
    mr.pkt = params;
    mr.attempts = 0;

    params->set("c", cmd);

    m_mustarrive_mutex.lock();
    mr.id = ++m_mustarrive_e;
    params->set("e", mr.id);
    m_mustarrive_queue.push_back(mr);
    m_mustarrive_mutex.unlock();

    send(&addr, port, params);
}

void Protocol::send(const char *cmd, Object *obj) {
    struct ip_addr addr;
    u16_t port;
    if(!get_possessed_addr(&addr, &port)) {
        ESP_LOGW(TAG, "can't send, the device was not possessed yet.");
        return;
    }
    send(&addr, port, cmd, obj);
}

void Protocol::send(const ip_addr_t *addr, u16_t port, const char *cmd, Object *obj) {
    std::unique_ptr<Object> autoptr;
    if(obj == NULL) {
        obj = new Object();
        autoptr.reset(obj);
    }

    obj->set("c", new String(cmd));
    send(addr, port, obj);
}

void Protocol::send(const ip_addr_t *addr, u16_t port, Object *obj) {
    m_mutex.lock();
    int n = m_write_counter++;
    m_mutex.unlock();

    obj->set("n", new Number(n));
    auto str = obj->str();
    send(addr, port, str.c_str(), str.size());
}

void Protocol::send(const ip_addr_t *addr, u16_t port, const char *buf) {
    send(addr, port, buf, strlen(buf));
}

void Protocol::send(const ip_addr_t *addr, u16_t port, const char *buf, size_t size) {
    SendQueueItem it;
    ip_addr_copy(it.addr, *addr);
    it.port = port;
    it.buf = pbuf_alloc(PBUF_TRANSPORT, size, PBUF_RAM);
    pbuf_take(it.buf, buf, size);

    if(xQueueSend(m_sendQueue, &it, 200 / portTICK_PERIOD_MS) != pdTRUE) {
        ESP_LOGE(TAG, "failed to send - queue full!");
    }
}

void Protocol::send_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    send_log(fmt, args);
    va_end(args);
}

void Protocol::send_log(const char *fmt, va_list args) {
    char static_buf[128];
    std::unique_ptr<char[]> dyn_buf;
    char *used_buf = static_buf;

    int fmt_len = vsnprintf(static_buf, sizeof(static_buf), fmt, args);
    if(fmt_len >= sizeof(static_buf)) {
        dyn_buf.reset(new char[fmt_len+1]);
        used_buf = dyn_buf.get();
        vsnprintf(dyn_buf.get(), fmt_len+1, fmt, args);
    }

    Object *pkt = new Object();
    pkt->set("msg", used_buf);
    send_mustarrive("log", pkt);
}

void Protocol::send_task_trampoline(void *ctrl) {
    ((Protocol*)ctrl)->send_task();
}

void Protocol::send_task() {
    auto *pcb = udp_new();

    udp_recv(pcb, recv_trampoline, this);

    const auto bind_res = udp_bind(pcb, IP_ADDR_ANY, m_port);
    if(bind_res != ERR_OK) {
        ESP_LOGE(TAG, "failed to call udp_bind: %d", (int)bind_res);
        return;
    }

    uint32_t mustarrive_timer = MUST_ARRIVE_TIMER_PERIOD;
    struct timeval tv_last, tv_now;
    gettimeofday(&tv_last, NULL);

    SendQueueItem it;

    while(true) {
        while(xQueueReceive(m_sendQueue, &it, 10 / portTICK_PERIOD_MS) == pdTRUE) {
            if(it.buf == nullptr) {
                goto exit;
            }

            const auto send_res = udp_sendto(pcb, it.buf, &it.addr, it.port);
            if(send_res != ERR_OK) {
                ESP_LOGE(TAG, "error in udp_sendto: %d!", int(send_res));
            }
            pbuf_free(it.buf);
        }

        gettimeofday(&tv_now, NULL);
        uint32_t diff = diff_ms(tv_now, tv_last);
        memcpy(&tv_last, &tv_now, sizeof(struct timeval));

        if(mustarrive_timer <= diff) {
            m_mustarrive_mutex.lock();
            if(m_mustarrive_queue.size() != 0) {
                resend_mustarrive_locked();
            }
            m_mustarrive_mutex.unlock();
            mustarrive_timer = MUST_ARRIVE_TIMER_PERIOD;
        } else {
            mustarrive_timer -= diff;
        }
    }

exit:
    udp_disconnect(pcb);
    udp_remove(pcb);

    vTaskDelete(nullptr);
}

void Protocol::resend_mustarrive_locked() {
    struct ip_addr addr;
    u16_t port;

    m_mutex.lock();
    ip_addr_copy(addr, m_possessed_addr);
    port = m_possessed_port;
    m_mutex.unlock();

    for(auto itr = m_mustarrive_queue.begin(); itr != m_mustarrive_queue.end(); ) {
        send(&addr, port, (*itr).pkt);
        if((*itr).attempts != -1 && ++(*itr).attempts >= MUST_ARRIVE_ATTEMPTS) {
            delete (*itr).pkt;
            itr = m_mustarrive_queue.erase(itr);
        } else {
            ++itr;
        }
    }
}

void Protocol::recv_trampoline(void *ctrl, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    RecvQueueItem it;
    it.port = port;
    ip_addr_copy(it.addr, *addr);

    it.size = p->tot_len;
    it.buf = (char*)malloc(it.size);
    pbuf_copy_partial(p, it.buf, it.size, 0);
    pbuf_free(p);

    if(xQueueSend(((Protocol*)ctrl)->m_recvQueue, &it, 200 / portTICK_PERIOD_MS) != pdTRUE) {
        ESP_LOGE(TAG, "failed to recv - queue full!");
    }
}

void Protocol::recv_task_trampoline(void *ctrl) {
    ((Protocol*)ctrl)->recv_task();
}

void Protocol::recv_task() {
    RecvQueueItem it;
    while(true) {
        if(xQueueReceive(m_recvQueue, &it, portMAX_DELAY) == pdTRUE) {
            if(it.buf == nullptr)
                break;
            handle_msg(&it.addr, it.port, it.buf, it.size);
            free(it.buf);
        }
    }

    vTaskDelete(nullptr);
}

void Protocol::handle_msg(const ip_addr_t *addr, u16_t port, char *buf, ssize_t size) {
    std::unique_ptr<Object> pkt(parse(buf, size));
    if(!pkt) {
        ESP_LOGE(TAG, "failed to parse the packet's json");
        return;
    }

    auto cmd = pkt->getString("c");

    ESP_LOGD(TAG, "Got command: %s %.*s", cmd.c_str(), size, buf);

    if(cmd == "discover") {
        std::unique_ptr<Object> res(new Object());
        res->set("c", "found");
        res->set("owner", m_owner);
        res->set("name", m_name);
        res->set("desc", m_desc);

        const auto str = res->str();
        send(addr, port, str.c_str(), str.size());
        return;
    }

    if(!pkt->contains("n")) {
        ESP_LOGE(TAG, "packet does not have counter %s", buf);
        return;
    }

    int counter = pkt->getInt("n");
    if(counter == -1) {
        m_read_counter = 0;
        m_mutex.lock();
        m_write_counter = 0;
        m_mutex.unlock();
    } else if(counter < m_read_counter && m_read_counter - counter < 300) {
        return;
    } else {
        m_read_counter = counter;
    }

    if(pkt->contains("f")) {
        send(addr, port, pkt.get());

        if(cmd == "possess") {
            m_mutex.lock();
            bool different = ip_addr_cmp(&m_possessed_addr, addr) != 0 || m_possessed_port != port;
            if(different) {
                ip_addr_copy(m_possessed_addr, *addr);
                m_possessed_port = port;
                m_mustarrive_e = 0;
                m_mustarrive_f = 0;
            }
            m_mutex.unlock();

            m_mustarrive_mutex.lock();
            for(auto it : m_mustarrive_queue) {
                delete it.pkt;
            }
            m_mustarrive_queue.clear();
            m_mustarrive_mutex.unlock();
        }

        int f = pkt->getInt("f");
        if(f <= m_mustarrive_f) {
            return;
        } else {
            m_mustarrive_f = f;
        }
    } else if(pkt->contains("e")) {
        int e = pkt->getInt("e");
        m_mustarrive_mutex.lock();
        for(auto itr = m_mustarrive_queue.begin(); itr != m_mustarrive_queue.end(); ++itr) {
            if((*itr).id == e) {
                delete (*itr).pkt;
                m_mustarrive_queue.erase(itr);
                break;
            }
        }
        m_mustarrive_mutex.unlock();
        return;
    }

    if(cmd == "possess") {
        ESP_LOGI(TAG, "We are possessed!");
        send_log("The device %s has been possessed!\n", m_name);
    } else if(m_callback != NULL) {
        m_callback(cmd, pkt.get());
    }
}


}; // namespace rb

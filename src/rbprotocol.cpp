#include <esp_log.h>
#include <cstring>

#include "rbprotocol.h"
#include "rbprotocoludp.h"
#include "rbprotocolws.h"

#define RBPROT_TAG "RbProtocol"

#define MS_TO_TICKS(ms) ((portTICK_PERIOD_MS <= ms) ? (ms / portTICK_PERIOD_MS) : 1)

#define MUST_ARRIVE_TIMER_PERIOD MS_TO_TICKS(100)
#define MUST_ARRIVE_ATTEMPTS 15

namespace rb {

using namespace rb::internal;

const ProtocolConfig Protocol::DEFAULT_CONFIG = {
    .enable_udp = true,
    .enable_ws = true,
    .ws_register_with_webserver = true,
    .udp_port = 42424,
};

Protocol::Protocol(const char* owner, const char* name, const char* description, Protocol::callback_t callback) {
    m_owner = owner;
    m_name = name;
    m_desc = description;
    m_callback = callback;

    m_sendQueue = xQueueCreate(32, sizeof(QueueItem));

    m_read_counter = 0;
    m_write_counter = 0;

    m_mustarrive_e = 0;
    m_mustarrive_f = 0xFFFFFFFF;

    m_task_send = nullptr;
    m_task_recv = nullptr;

    m_udp = nullptr;
    m_ws = nullptr;

    memset(&m_possessed_addr, 0, sizeof(m_possessed_addr));
}

Protocol::~Protocol() {
    stop();
    vQueueDelete(m_sendQueue);
}

esp_err_t Protocol::start(const ProtocolConfig& cfg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_task_send != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!cfg.enable_udp && !cfg.enable_ws) {
        ESP_LOGE(RBPROT_TAG, "One of enable_udp, enable_ws must be true!");
        return ESP_ERR_INVALID_ARG;
    }

    std::unique_ptr<ProtBackendUdp> udp;
    std::unique_ptr<ProtBackendWs> ws;
    esp_err_t err;

    if (cfg.enable_udp) {
        udp.reset(new ProtBackendUdp());
        err = udp->start(cfg.udp_port);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (cfg.enable_ws) {
        ws.reset(new ProtBackendWs());
        err = ws->start(cfg.ws_register_with_webserver);
        if (err != ESP_OK) {
            return err;
        }
    }

    m_udp = udp.release();
    m_ws = ws.release();

    xTaskCreate(&Protocol::send_task, "rbctrl_send", 2560, this, 9, &m_task_send);
    xTaskCreate(&Protocol::recv_task, "rbctrl_recv", 4096, this, 10, &m_task_recv);
    return ESP_OK;
}

void Protocol::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_task_send == nullptr) {
        return;
    }

    QueueItem it = {};
    xQueueSendToFront(m_sendQueue, &it, portMAX_DELAY);
    xTaskNotify(m_task_recv, 0, eNoAction);

    delete m_udp;
    delete m_ws;
    m_udp = nullptr;
    m_ws = nullptr;

    m_task_send = nullptr;
    m_task_recv = nullptr;
}

bool Protocol::is_addr_empty(const ProtocolAddr& addr) const {
    return addr.kind == ProtBackendType::PROT_NONE;
}

bool Protocol::is_addr_same(const ProtocolAddr& a, const internal::ProtocolAddr& b) const {
    return memcmp(&a, &b, sizeof(ProtocolAddr)) == 0;
}

bool Protocol::get_possessed_addr(ProtocolAddr& addr) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (is_addr_empty(m_possessed_addr))
        return false;
    addr = m_possessed_addr;
    return true;
}

bool Protocol::is_possessed() const {
    m_mutex.lock();
    bool res = !is_addr_empty(m_possessed_addr);
    m_mutex.unlock();
    return res;
}

bool Protocol::is_mustarrive_complete(uint32_t id) const {
    if (id == UINT32_MAX)
        return true;

    std::lock_guard<std::mutex> l(m_mustarrive_mutex);
    for (const auto& it : m_mustarrive_queue) {
        if (it.id == id)
            return false;
    }
    return true;
}

void Protocol::send(const char* cmd, rbjson::Object* obj) {
    ProtocolAddr addr;
    if (!get_possessed_addr(addr)) {
        ESP_LOGW(RBPROT_TAG, "can't send, the device was not possessed yet.");
        return;
    }
    send(addr, cmd, obj);
}

void Protocol::send(const ProtocolAddr& addr, const char* cmd, rbjson::Object* obj) {
    std::unique_ptr<rbjson::Object> autoptr;
    if (obj == NULL) {
        obj = new rbjson::Object();
        autoptr.reset(obj);
    }

    obj->set("c", new rbjson::String(cmd));
    send(addr, obj);
}

void Protocol::send(const ProtocolAddr& addr, rbjson::Object* obj) {
    m_mutex.lock();
    const int n = m_write_counter++;
    m_mutex.unlock();

    obj->set("n", new rbjson::Number(n));
    const auto str = obj->str();
    send(addr, str.c_str(), str.size());
}

void Protocol::send(const ProtocolAddr& addr, const char* buf) {
    send(addr, buf, strlen(buf));
}

void Protocol::send(const ProtocolAddr& addr, const char* buf, size_t size) {
    if (size == 0)
        return;

    QueueItem it;
    it.addr = addr;
    it.buf = new char[size];
    it.size = size;
    memcpy(it.buf, buf, size);

    if (xQueueSend(m_sendQueue, &it, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(RBPROT_TAG, "failed to send - queue full!");
        delete[] it.buf;
    }
}

uint32_t Protocol::send_mustarrive(const char* cmd, rbjson::Object* params) {
    ProtocolAddr addr;
    if (!get_possessed_addr(addr)) {
        ESP_LOGW(RBPROT_TAG, "can't send, the device was not possessed yet.");
        delete params;
        return UINT32_MAX;
    }

    if (params == NULL) {
        params = new rbjson::Object();
    }

    params->set("c", cmd);

    MustArrive mr;
    mr.pkt = params;
    mr.attempts = 0;

    m_mustarrive_mutex.lock();
    const uint32_t id = m_mustarrive_e++;
    mr.id = id;
    params->set("e", mr.id);
    m_mustarrive_queue.emplace_back(mr);
    send(addr, params);
    m_mustarrive_mutex.unlock();

    return id;
}

void Protocol::send_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    send_log(fmt, args);
    va_end(args);
}

void Protocol::send_log(const char* fmt, va_list args) {
    char static_buf[256];
    std::unique_ptr<char[]> dyn_buf;
    char* used_buf = static_buf;

    int fmt_len = vsnprintf(static_buf, sizeof(static_buf), fmt, args);
    if (fmt_len >= sizeof(static_buf)) {
        dyn_buf.reset(new char[fmt_len + 1]);
        used_buf = dyn_buf.get();
        vsnprintf(dyn_buf.get(), fmt_len + 1, fmt, args);
    }

    send_log(std::string(used_buf));
}

void Protocol::send_log(const std::string& str) {
    rbjson::Object* pkt = new rbjson::Object();
    pkt->set("msg", str);
    send_mustarrive("log", pkt);
}

void Protocol::handle_msg(const ProtocolAddr& addr, rbjson::Object* pkt) {
    const auto cmd = pkt->getString("c");

    if (cmd == "discover") {
        std::unique_ptr<rbjson::Object> res(new rbjson::Object());
        res->set("c", "found");
        res->set("owner", m_owner);
        res->set("name", m_name);
        res->set("desc", m_desc);

        const auto str = res->str();
        send(addr, str.c_str(), str.size());
        return;
    }

    if (!pkt->contains("n")) {
        ESP_LOGE(RBPROT_TAG, "packet does not have counter!");
        return;
    }

    const bool isPossessCmd = cmd == "possess";

    const int counter = pkt->getInt("n");
    if (counter == -1 || isPossessCmd) {
        m_read_counter = 0;
        m_mutex.lock();
        m_write_counter = 0;
        m_mutex.unlock();
    } else if (counter < m_read_counter && m_read_counter - counter < 25) {
        return;
    } else {
        m_read_counter = counter;
    }

    if (is_addr_empty(m_possessed_addr) || isPossessCmd) {
        m_mutex.lock();
        if (!is_addr_same(m_possessed_addr, addr)) {
            m_possessed_addr = addr;
        }
        m_mustarrive_e = 0;
        m_mustarrive_f = 0xFFFFFFFF;
        m_write_counter = -1;
        m_read_counter = -1;
        m_mutex.unlock();

        m_mustarrive_mutex.lock();
        for (auto it : m_mustarrive_queue) {
            delete it.pkt;
        }
        m_mustarrive_queue.clear();
        m_mustarrive_mutex.unlock();
    }

    if (pkt->contains("f")) {
        {
            std::unique_ptr<rbjson::Object> resp(new rbjson::Object);
            resp->set("c", cmd);
            resp->set("f", pkt->getInt("f"));
            send(addr, resp.get());
        }

        int f = pkt->getInt("f");
        if (f <= m_mustarrive_f && m_mustarrive_f != 0xFFFFFFFF) {
            return;
        } else {
            m_mustarrive_f = f;
        }
    } else if (pkt->contains("e")) {
        uint32_t e = pkt->getInt("e");
        m_mustarrive_mutex.lock();
        for (auto itr = m_mustarrive_queue.begin(); itr != m_mustarrive_queue.end(); ++itr) {
            if ((*itr).id == e) {
                delete (*itr).pkt;
                m_mustarrive_queue.erase(itr);
                break;
            }
        }
        m_mustarrive_mutex.unlock();
        return;
    }

    if (isPossessCmd) {
        ESP_LOGI(RBPROT_TAG, "We are possessed!");
        send_log("The device %s has been possessed!\n", m_name);
    }

    if (m_callback != NULL) {
        m_callback(cmd, pkt);
    }
}

void Protocol::resend_mustarrive_locked() {
    ProtocolAddr possesed_addr;
    get_possessed_addr(possesed_addr);

    for (auto itr = m_mustarrive_queue.begin(); itr != m_mustarrive_queue.end();) {
        // Websocket protocol does not resend
        if (possesed_addr.kind == ProtBackendType::PROT_UDP) {
            m_mutex.lock();
            if (m_udp) {
                itr->pkt->set("n", m_write_counter++);
                m_udp->resend_mustarrive(possesed_addr, itr->pkt);
            }
            m_mutex.unlock();
        }

        if (++(*itr).attempts >= MUST_ARRIVE_ATTEMPTS) {
            delete (*itr).pkt;
            itr = m_mustarrive_queue.erase(itr);
        } else {
            ++itr;
        }
    }
}

void Protocol::send_task(void* selfVoid) {
    auto& self = *((Protocol*)selfVoid);

    TickType_t mustarrive_next;
    QueueItem it;

    mustarrive_next = xTaskGetTickCount() + MUST_ARRIVE_TIMER_PERIOD;

    while (true) {
        for (uint8_t i = 0; i < 16 && xQueueReceive(self.m_sendQueue, &it, MS_TO_TICKS(10)) == pdTRUE; ++i) {
            if (it.addr.kind == ProtBackendType::PROT_NONE) {
                goto exit;
            }

            self.m_mutex.lock();
            switch (it.addr.kind) {
            case ProtBackendType::PROT_UDP:
                if (self.m_udp) {
                    self.m_udp->send_from_queue(it);
                }
                break;
            case ProtBackendType::PROT_WS:
                if (self.m_ws) {
                    self.m_ws->send_from_queue(it);
                }
                break;
            case ProtBackendType::PROT_NONE:
                break;
            }
            self.m_mutex.unlock();

            delete[] it.buf;
        }

        if (xTaskGetTickCount() >= mustarrive_next) {
            self.m_mustarrive_mutex.lock();
            if (self.m_mustarrive_queue.size() != 0) {
                self.resend_mustarrive_locked();
            }
            self.m_mustarrive_mutex.unlock();
            mustarrive_next = xTaskGetTickCount() + MUST_ARRIVE_TIMER_PERIOD;
        }
    }

exit:
    vTaskDelete(nullptr);
}

void Protocol::recv_task(void* selfVoid) {
    auto& self = *((Protocol*)selfVoid);

    {
        // destructors do not run after vTaskDelete, so put this vector in separate block to enforce it
        std::vector<uint8_t> buf;
        buf.resize(64);

        ProtocolAddr recv_addr;

        while (xTaskNotifyWait(0, 0, NULL, 0) == pdFALSE) {
            bool received_msg = false;

            self.m_mutex.lock();
            if (self.m_udp) {
                auto pkt = self.m_udp->recv_iter(buf, recv_addr);
                if (pkt) {
                    self.m_mutex.unlock();
                    self.handle_msg(recv_addr, pkt.get());
                    pkt.reset();
                    received_msg = true;
                    self.m_mutex.lock();
                }
            }

            if (self.m_ws) {
                auto pkt = self.m_ws->recv_iter(buf, recv_addr);
                if (pkt) {
                    self.m_mutex.unlock();
                    self.handle_msg(recv_addr, pkt.get());
                    pkt.reset();
                    received_msg = true;
                } else {
                    self.m_mutex.unlock();
                }
            } else {
                self.m_mutex.unlock();
            }

            if (!received_msg) {
                vTaskDelay(MS_TO_TICKS(10));
            }
        }
    }

    vTaskDelete(nullptr);
}

};

#pragma once 

#include <stdarg.h>
#include <stdio.h>
#include <functional>
#include <memory>
#include <mutex>
#include <esp_log.h>
#include <cstring>

#include "rbjson.h"

#define RBPROT_TAG "RbProtocol"

namespace rb {


class ProtocolBase {
public:
    virtual ~ProtocolBase() {}

    virtual void send(const char* cmd, rbjson::Object* params = NULL) = 0;
    virtual uint32_t send_mustarrive(const char* cmd, rbjson::Object* params = NULL) = 0;

    virtual bool is_possessed() const = 0; //!< Returns true of the device is possessed (somebody connected to it)
    virtual bool is_mustarrive_complete(uint32_t id) const = 0;

    void send_log(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        send_log(fmt, args);
        va_end(args);
    }

    void send_log(const char* fmt, va_list args) {
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

    void send_log(const std::string& str) {
        rbjson::Object* pkt = new rbjson::Object();
        pkt->set("msg", str);
        send_mustarrive("log", pkt);
    }
};

template<typename AddrT>
class ProtocolImplBase : public ProtocolBase {
public:
    typedef std::function<void(const std::string& cmd, rbjson::Object* pkt)> callback_t;

    ProtocolImplBase(const char* owner, const char* name, const char* description, callback_t callback = nullptr);
    virtual ~ProtocolImplBase() { 
        stop();
    }

    virtual void stop();

    void send(const char* cmd, rbjson::Object* params = NULL);
    uint32_t send_mustarrive(const char* cmd, rbjson::Object* params = NULL);

    bool is_possessed() const; //!< Returns true of the device is possessed (somebody connected to it)
    bool is_mustarrive_complete(uint32_t id) const;

    TaskHandle_t getTaskSend() const { return m_task_send; }
    TaskHandle_t getTaskRecv() const { return m_task_recv; }

protected:
    struct MustArrive {
        rbjson::Object* pkt;
        uint32_t id;
        int16_t attempts;
    };

    struct QueueItem {
        AddrT addr;
        char* buf;
        uint16_t size;
    };

    void handle_msg(const AddrT& addr, rbjson::Object* pkt);

    bool get_possessed_addr(AddrT& addr) const;
    virtual bool is_addr_empty(const AddrT& addr) const = 0;
    virtual bool is_addr_same(const AddrT& a, const AddrT& b) const = 0;

    void send(const AddrT& addr, const char* command, rbjson::Object* obj);
    void send(const AddrT& addr, rbjson::Object* obj);
    void send(const AddrT& addr, const char* buf);
    void send(const AddrT& addr, const char* buf, size_t size);

    const char* m_owner;
    const char* m_name;
    const char* m_desc;

    callback_t m_callback;

    int32_t m_read_counter;
    int32_t m_write_counter;
    AddrT m_possessed_addr;
    QueueHandle_t m_sendQueue;
    mutable std::mutex m_mutex;

    uint32_t m_mustarrive_e;
    uint32_t m_mustarrive_f;
    std::vector<MustArrive> m_mustarrive_queue;
    mutable std::mutex m_mustarrive_mutex;

    TaskHandle_t m_task_send;
    TaskHandle_t m_task_recv;
};


template<typename AddrT>
ProtocolImplBase<AddrT>::ProtocolImplBase(const char* owner, const char* name, const char* description, ProtocolImplBase<AddrT>::callback_t callback) {
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
}

template<typename AddrT>
void ProtocolImplBase<AddrT>::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_task_send == nullptr) {
        return;
    }

    QueueItem it = { };
    xQueueSend(m_sendQueue, &it, portMAX_DELAY);

    m_task_send = nullptr;
    m_task_recv = nullptr;
}


template<typename AddrT>
bool ProtocolImplBase<AddrT>::get_possessed_addr(AddrT& addr) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (is_addr_empty(m_possessed_addr))
        return false;
    addr = m_possessed_addr;
    return true;
}

template<typename AddrT>
bool ProtocolImplBase<AddrT>::is_possessed() const {
    m_mutex.lock();
    bool res = !is_addr_empty(m_possessed_addr);
    m_mutex.unlock();
    return res;
}

template<typename AddrT>
bool ProtocolImplBase<AddrT>::is_mustarrive_complete(uint32_t id) const {
    if (id == UINT32_MAX)
        return true;

    std::lock_guard<std::mutex> l(m_mustarrive_mutex);
    for (const auto& it : m_mustarrive_queue) {
        if (it.id == id)
            return false;
    }
    return true;
}

template<typename AddrT>
void ProtocolImplBase<AddrT>::send(const char* cmd, rbjson::Object* obj) {
    AddrT addr;
    if (!get_possessed_addr(addr)) {
        ESP_LOGW(RBPROT_TAG, "can't send, the device was not possessed yet.");
        return;
    }
    send(addr, cmd, obj);
}

template<typename AddrT>
void ProtocolImplBase<AddrT>::send(const AddrT& addr, const char* cmd, rbjson::Object* obj) {
    std::unique_ptr<rbjson::Object> autoptr;
    if (obj == NULL) {
        obj = new rbjson::Object();
        autoptr.reset(obj);
    }

    obj->set("c", new rbjson::String(cmd));
    send(addr, obj);
}

template<typename AddrT>
void ProtocolImplBase<AddrT>::send(const AddrT& addr, rbjson::Object* obj) {
    m_mutex.lock();
    const int n = m_write_counter++;
    m_mutex.unlock();

    obj->set("n", new rbjson::Number(n));
    const auto str = obj->str();
    send(addr, str.c_str(), str.size());
}

template<typename AddrT>
void ProtocolImplBase<AddrT>::send(const AddrT& addr, const char* buf) {
    send(addr, buf, strlen(buf));
}

template<typename AddrT>
void ProtocolImplBase<AddrT>::send(const AddrT& addr, const char* buf, size_t size) {
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

template<typename AddrT>
uint32_t ProtocolImplBase<AddrT>::send_mustarrive(const char* cmd, rbjson::Object* params) {
    AddrT addr;
    if (!get_possessed_addr(addr)) {
        ESP_LOGW(RBPROT_TAG, "can't send, the device was not possessed yet.");
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

template<typename AddrT>
void ProtocolImplBase<AddrT>::handle_msg(const AddrT& addr, rbjson::Object* pkt) {
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

};

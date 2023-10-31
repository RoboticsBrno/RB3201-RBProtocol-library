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
#include "rbprotocolws.h"

#include "rbwebserver_internal.h"

using namespace rbjson;

#define MS_TO_TICKS(ms) ((portTICK_PERIOD_MS <= ms) ? (ms / portTICK_PERIOD_MS) : 1)

#define MUST_ARRIVE_TIMER_PERIOD MS_TO_TICKS(100)
#define MUST_ARRIVE_ATTEMPTS 15

#define WS_OPCODE_CONTINUE 0x00
#define WS_OPCODE_TEXT 0x01
#define WS_OPCODE_CLOSE 0x08

namespace rb {

ProtocolWs::ProtocolWs(const char* owner, const char* name, const char* description, ProtocolWs::callback_t callback) :
    ProtocolImplBase(owner, name, description, callback) {
    m_task_send = nullptr;
    m_task_recv = nullptr;

    memset(&m_possessed_addr, 0, sizeof(WsAddr));

    xTaskCreate(&ProtocolWs::recv_task_trampoline, "rbctrl_recv", 4096, this, 10, &m_task_recv);
    xTaskCreate(&ProtocolWs::send_task_trampoline, "rbctrl_send", 2560, this, 9, &m_task_send);
}

void ProtocolWs::start() {
    rb_web_set_wsprotocol(this);
}

void ProtocolWs::stop() {
    if(m_task_recv) {
        rb_web_clear_wsprotocol(NULL);
        xTaskNotify(m_task_recv, 0, eNoAction);
    }

    ProtocolImplBase::stop();

    m_client_fd_mu.lock();
    for(auto& client : m_client_fd) {
        close(client->fd);
    }
    m_client_fd.clear();
    m_client_fd_mu.unlock();
}

void ProtocolWs::addClient(int fd) {
    m_client_fd_mu.lock();
    m_client_fd.push_back(std::unique_ptr<Client>(new Client(fd)));
    m_client_fd_mu.unlock();
}

void ProtocolWs::close_client(int fd) {
    m_client_fd_mu.lock();
    for(auto itr = m_client_fd.begin(); itr != m_client_fd.end(); ++itr) {
        if(itr->get()->fd == fd) {
            itr = m_client_fd.erase(itr);
            close(fd);
            break;
        }
    }
    m_client_fd_mu.unlock();
}

void ProtocolWs::send_task_trampoline(void* ctrl) {
    ((ProtocolWs*)ctrl)->send_task();
}

void ProtocolWs::send_task() {
    TickType_t mustarrive_next;
    QueueItem it;

    uint8_t ws_header[4];

    mustarrive_next = xTaskGetTickCount() + MUST_ARRIVE_TIMER_PERIOD;

    while (true) {
        for (size_t i = 0; xQueueReceive(m_sendQueue, &it, MS_TO_TICKS(10)) == pdTRUE && i < 16; ++i) {
            if (it.buf == nullptr) {
                if(it.addr.fd == 0) {
                    goto exit;
                }

                ws_header[0] = (1 << 7) | WS_OPCODE_CLOSE;
            } else {
                ws_header[0] = (1 << 7) | WS_OPCODE_TEXT; // FIN flag + text opcode
            }

            if(it.size <= 125) {
                ws_header[1] = it.size;
            } else {
                ws_header[1] = 126;
                ws_header[2] = it.size >> 8;
                ws_header[3] = it.size & 0xFF;
            }

            int res = ::send(it.addr.fd, ws_header, it.size <= 125 ? 2 : 4, 0);
            if (res < 0) {
                ESP_LOGE(RBPROT_TAG, "error in sendto: %d %s!", errno, strerror(errno));
                close_client(it.addr.fd);
                delete[] it.buf;
                continue;
            }

            if(it.buf == nullptr) {
                close_client(it.addr.fd);
                continue;
            }

            res = ::send(it.addr.fd, it.buf, it.size, 0);
            if (res < 0) {
                ESP_LOGE(RBPROT_TAG, "error in sendto: %d %s!", errno, strerror(errno));
                close_client(it.addr.fd);
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

void ProtocolWs::resend_mustarrive_locked() {
    for (auto itr = m_mustarrive_queue.begin(); itr != m_mustarrive_queue.end();) {
        if ((*itr).attempts != -1 && ++(*itr).attempts >= MUST_ARRIVE_ATTEMPTS) {
            delete (*itr).pkt;
            itr = m_mustarrive_queue.erase(itr);
        } else {
            ++itr;
        }
    }
}

void ProtocolWs::recv_task_trampoline(void* ctrl) {
    ((ProtocolWs*)ctrl)->recv_task();
}

static int read_at_least(int fd, uint8_t *buf, size_t n) {
    int res = ::recv(fd, buf, n, MSG_PEEK);
    if(res < 0) {
        if(errno == EWOULDBLOCK) {
            return 0;
        } else {
            ESP_LOGE(RBPROT_TAG, "WS client %d returned error %d (%s) when calling recv, closing", fd, errno, strerror(errno));
            return -1;
        }
    }

    if(res < n) {
        return 0;
    }

    return ::recv(fd, buf, n, 0);
}

int ProtocolWs::process_client_header(Client &client, std::vector<uint8_t>& buf) {
    client.flags = buf[0];

    const int mask = buf[1] >> 7;
    const uint8_t len0 = buf[1] & 0x7f;

    if(!mask) {
        ESP_LOGE(RBPROT_TAG, "WS client %d returned sent unmasked frame, closing", client.fd);
        return -1;
    }

    if(len0 >= 127) {
        ESP_LOGE(RBPROT_TAG, "WS client %d sent message with 64bit length, closing", client.fd);
        return -1;
    }

    ESP_LOGV(RBPROT_TAG, "WS client %d got header with len %d", client.fd, len0);

    if(len0 < 126) {
        client.remaining_payload_len = len0;
        client.state = ClientState::MASK;
    } else {
        client.state = ClientState::LEN1;
    }

    if(client.opcode() != WS_OPCODE_CONTINUE) {
        client.payload.resize(0);
    }
    return 0;
}

int ProtocolWs::process_client(Client &client, std::vector<uint8_t>& buf) {
    switch(client.state) {
    case ClientState::INITIAL: {
        int n = read_at_least(client.fd, buf.data(), 2);
        if(n > 0) {
            n = process_client_header(client, buf);
        }
        return n;
    }
    case ClientState::LEN1: {
        int n = read_at_least(client.fd, buf.data(), 2);
        if(n > 0) {
            client.remaining_payload_len = (buf[0] << 8) | buf[1];
            client.state = ClientState::MASK;
            ESP_LOGV(RBPROT_TAG, "WS client %d got extra len %d", client.fd, client.remaining_payload_len);
        }
        return n;
    }
    case ClientState::MASK: {
        int n = read_at_least(client.fd, client.masking_key, 4);
        if(n <= 0) {
            return n;
        }

        ESP_LOGV(RBPROT_TAG, "WS client %d got mask %x%x%x%x", client.fd,
            client.masking_key[0], client.masking_key[1], client.masking_key[2], client.masking_key[3]);

        if(client.remaining_payload_len == 0) {
            client.state = client.fin() ? ClientState::FULLY_RECEIVED : ClientState::INITIAL;
        } else {
            const size_t total_payload_size = client.payload.size() + client.remaining_payload_len;
            if(total_payload_size > 32*1024) {
                ESP_LOGE(RBPROT_TAG, "WS client %d sent too long message, %u", client.fd, total_payload_size);
                return -1;
            }

            client.payload.resize(total_payload_size);
            client.state = ClientState::DATA;
        }
        return n;
    }
    case ClientState::DATA: {
        while(true) {
            const int16_t n = read_at_least(client.fd, buf.data(), std::min(client.remaining_payload_len, (uint16_t)64));
            if(n <= 0) {
                return n;
            }

            uint16_t payload_idx = client.payload.size() - client.remaining_payload_len;
            for(int16_t i = 0; i < n; ++i) {
                client.payload[payload_idx] = buf[i] ^ client.masking_key[i%4];
                ++payload_idx;
            }
            client.remaining_payload_len -= n;

            if(client.remaining_payload_len == 0) {
                if(client.fin()) {
                    client.state = ClientState::FULLY_RECEIVED;
                } else {
                    client.state = ClientState::INITIAL;
                }
                return 0;
            }
        }
        break;
    }
    case ClientState::FULLY_RECEIVED:
        // unreachable
        client.state = ClientState::INITIAL;
        break;
    }
    return 0;
}

void ProtocolWs::process_client_fully_received_locked(Client &client) {
    const WsAddr sa = { .fd = client.fd };

    if(client.opcode() == WS_OPCODE_CLOSE) {
        const QueueItem it = {
            .addr = sa,
            .buf = nullptr,
            .size = 0
        };
        xQueueSend(m_sendQueue, &it, portMAX_DELAY);
    } else {
        ESP_LOGV(RBPROT_TAG, "parsing message %d %.*s", sa.fd, client.payload.size(), (char*)client.payload.data());

        std::unique_ptr<Object> pkt(parse((char*)client.payload.data(), client.payload.size()));
        if (!pkt) {
            ESP_LOGE(RBPROT_TAG, "failed to parse the packet's json");
            return;
        }

        m_client_fd_mu.unlock();
        handle_msg(sa, pkt.get());
        m_client_fd_mu.lock();
    }
    
}

void ProtocolWs::recv_task() {
    std::vector<uint8_t> buf;
    buf.resize(64);

    while(xTaskNotifyWait(0, 0, NULL, 0) == pdFALSE) {
        bool some_fully_received = false;

        m_client_fd_mu.lock();
        for(auto itr = m_client_fd.begin(); itr != m_client_fd.end();) {
            auto& client = *itr->get();

            if(process_client(client, buf) < 0) {
                close(client.fd);
                itr = m_client_fd.erase(itr);
                continue;
            } else {
                some_fully_received |= (client.state == ClientState::FULLY_RECEIVED);
            }

            ++itr;
        }

        while(some_fully_received) {
            some_fully_received = false;
            for(auto& client : m_client_fd) {
                if(client->state != ClientState::FULLY_RECEIVED) {
                    continue;
                }
                some_fully_received = true;

                client->state = ClientState::INITIAL;
                process_client_fully_received_locked(*client.get());
                break;
            }
        }

        m_client_fd_mu.unlock();

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    vTaskDelete(nullptr);
}

extern "C" void rb_web_ws_new_connection(void *wsProtocolInstance, int fd) {
    if(wsProtocolInstance == NULL) {
        close(fd);
        return;
    }

    auto prot = (ProtocolWs*)wsProtocolInstance;
    prot->addClient(fd);
}

}; // namespace rb

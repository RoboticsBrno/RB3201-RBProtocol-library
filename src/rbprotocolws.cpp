#include <esp_log.h>
#include <cstring>

#include "rbprotocolws.h"
#include "rbwebserver_internal.h"

#define RBPROT_TAG "RBProtBackendWs"

#define WS_OPCODE_CONTINUE 0x00
#define WS_OPCODE_TEXT 0x01
#define WS_OPCODE_CLOSE 0x08

namespace rb {
namespace internal {

ProtBackendWs::ProtBackendWs() {
}

ProtBackendWs::~ProtBackendWs() {
    rb_web_clear_wsprotocol(this);

    m_clients_mu.lock();
    for (auto& client : m_clients) {
        close(client->fd);
    }
    m_clients.clear();
    m_clients_mu.unlock();
}

esp_err_t ProtBackendWs::start(bool register_with_webserver) {
    if (register_with_webserver) {
        rb_web_set_wsprotocol(this);
    }
    return ESP_OK;
}

void ProtBackendWs::send_from_queue(const QueueItem& it) {
    uint8_t ws_header[4];
    ws_header[0] = (1 << 7) | WS_OPCODE_TEXT; // FIN flag + text opcode

    if (it.size <= 125) {
        ws_header[1] = it.size;
    } else {
        ws_header[1] = 126;
        ws_header[2] = it.size >> 8;
        ws_header[3] = it.size & 0xFF;
    }

    int res = ::send(it.addr.ws.fd, ws_header, it.size <= 125 ? 2 : 4, 0);
    if (res < 0) {
        ESP_LOGE(RBPROT_TAG, "error in sendto: %d %s!", errno, strerror(errno));
        close_client(it.addr.ws.fd);
        return;
    }

    res = ::send(it.addr.ws.fd, it.buf, it.size, 0);
    if (res < 0) {
        ESP_LOGE(RBPROT_TAG, "error in sendto: %d %s!", errno, strerror(errno));
        close_client(it.addr.ws.fd);
    }
}

void ProtBackendWs::addClient(int fd) {
    m_clients_mu.lock();
    m_clients.push_back(std::unique_ptr<Client>(new Client(fd)));
    m_clients_mu.unlock();
}

void ProtBackendWs::close_client(int fd) {
    m_clients_mu.lock();
    close_client_locked(fd);
    m_clients_mu.unlock();
}

void ProtBackendWs::close_client_locked(int fd) {
    for (auto itr = m_clients.begin(); itr != m_clients.end(); ++itr) {
        if (itr->get()->fd == fd) {
            itr = m_clients.erase(itr);
            close(fd);
            return;
        }
    }
}

void ProtBackendWs::close_client_locked_gracefully(int fd) {
    const uint8_t ws_header[2] = {
        (1 << 7) | WS_OPCODE_CLOSE,
        0,
    };

    int res = ::send(fd, ws_header, 2, 0);
    if (res < 0) {
        ESP_LOGE(RBPROT_TAG, "error in sendto while gracefully closing: %d %s!", errno, strerror(errno));
    }

    close_client_locked(fd);
}

static int read_at_least(int fd, uint8_t* buf, size_t n) {
    int res = ::recv(fd, buf, n, MSG_PEEK | MSG_DONTWAIT);
    if (res < 0) {
        if (errno == EWOULDBLOCK) {
            return 0;
        } else {
            ESP_LOGE(RBPROT_TAG, "WS client %d returned error %d (%s) when calling recv, closing", fd, errno, strerror(errno));
            return -1;
        }
    }

    if (res < n) {
        return 0;
    }

    return ::recv(fd, buf, n, 0);
}

int ProtBackendWs::process_client_header(Client& client, std::vector<uint8_t>& buf) {
    client.flags = buf[0];

    const int mask = buf[1] >> 7;
    const uint8_t len0 = buf[1] & 0x7f;

    if (!mask) {
        ESP_LOGE(RBPROT_TAG, "WS client %d returned sent unmasked frame, closing", client.fd);
        return -1;
    }

    if (len0 >= 127) {
        ESP_LOGE(RBPROT_TAG, "WS client %d sent message with 64bit length, closing", client.fd);
        return -1;
    }

    ESP_LOGV(RBPROT_TAG, "WS client %d got header with len %d", client.fd, len0);

    if (len0 < 126) {
        client.remaining_payload_len = len0;
        client.state = ClientState::MASK;
    } else {
        client.state = ClientState::LEN1;
    }

    if (client.opcode() != WS_OPCODE_CONTINUE) {
        client.payload.resize(0);
    }
    return 0;
}

int ProtBackendWs::process_client(Client& client, std::vector<uint8_t>& buf) {
    switch (client.state) {
    case ClientState::INITIAL: {
        int n = read_at_least(client.fd, buf.data(), 2);
        if (n > 0) {
            n = process_client_header(client, buf);
        }
        return n;
    }
    case ClientState::LEN1: {
        int n = read_at_least(client.fd, buf.data(), 2);
        if (n > 0) {
            client.remaining_payload_len = (buf[0] << 8) | buf[1];
            client.state = ClientState::MASK;
            ESP_LOGV(RBPROT_TAG, "WS client %d got extra len %d", client.fd, client.remaining_payload_len);
        }
        return n;
    }
    case ClientState::MASK: {
        int n = read_at_least(client.fd, client.masking_key, 4);
        if (n <= 0) {
            return n;
        }

        ESP_LOGV(RBPROT_TAG, "WS client %d got mask %x%x%x%x", client.fd,
            client.masking_key[0], client.masking_key[1], client.masking_key[2], client.masking_key[3]);

        if (client.remaining_payload_len == 0) {
            client.state = client.fin() ? ClientState::FULLY_RECEIVED : ClientState::INITIAL;
        } else {
            const size_t total_payload_size = client.payload.size() + client.remaining_payload_len;
            if (total_payload_size > 32 * 1024) {
                ESP_LOGE(RBPROT_TAG, "WS client %d sent too long message, %u", client.fd, total_payload_size);
                return -1;
            }

            client.payload.resize(total_payload_size);
            client.state = ClientState::DATA;
        }
        return n;
    }
    case ClientState::DATA: {
        while (true) {
            const int16_t n = read_at_least(client.fd, buf.data(), std::min(client.remaining_payload_len, (uint16_t)64));
            if (n <= 0) {
                return n;
            }

            uint16_t payload_idx = client.payload.size() - client.remaining_payload_len;
            for (int16_t i = 0; i < n; ++i) {
                client.payload[payload_idx] = buf[i] ^ client.masking_key[i % 4];
                ++payload_idx;
            }
            client.remaining_payload_len -= n;

            if (client.remaining_payload_len == 0) {
                if (client.fin()) {
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

std::unique_ptr<rbjson::Object> ProtBackendWs::process_client_fully_received_locked(ProtBackendWs::Client& client, ProtocolAddr& out_received_addr) {
    client.state = ClientState::INITIAL;

    if (client.opcode() == WS_OPCODE_CLOSE) {
        close_client_locked_gracefully(client.fd);
        return nullptr;
    } else {
        ESP_LOGV(RBPROT_TAG, "parsing message %d %.*s", client.fd, client.payload.size(), (char*)client.payload.data());

        std::unique_ptr<rbjson::Object> pkt(rbjson::parse((char*)client.payload.data(), client.payload.size()));
        if (!pkt) {
            ESP_LOGE(RBPROT_TAG, "failed to parse the packet's json");
            return nullptr;
        }

        out_received_addr.kind = ProtBackendType::PROT_WS;
        out_received_addr.ws.fd = client.fd;
        return pkt;
    }
}

std::unique_ptr<rbjson::Object> ProtBackendWs::recv_iter(std::vector<uint8_t>& buf, ProtocolAddr& out_received_addr) {
    std::lock_guard<std::mutex> lock(m_clients_mu);

    for (auto itr = m_clients.begin(); itr != m_clients.end();) {
        auto& client = *itr->get();

        if (process_client(client, buf) < 0) {
            close(client.fd);
            itr = m_clients.erase(itr);
            continue;
        } else if (client.state == ClientState::FULLY_RECEIVED) {
            return process_client_fully_received_locked(client, out_received_addr);
        }

        ++itr;
    }

    return nullptr;
}

};
};

extern "C" void rb_web_ws_new_connection(void* wsProtocolInstance, int fd) {
    if (wsProtocolInstance == NULL) {
        close(fd);
        return;
    }

    auto prot = (rb::internal::ProtBackendWs*)wsProtocolInstance;
    prot->addClient(fd);
}

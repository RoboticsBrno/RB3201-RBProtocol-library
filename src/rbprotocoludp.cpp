#include <esp_log.h>

#include "rbprotocoludp.h"

#define RBPROT_TAG "RBProtBackendUdp"

namespace rb {
namespace internal {

ProtBackendUdp::ProtBackendUdp() {
    m_socket = -1;
}

ProtBackendUdp::~ProtBackendUdp() {
    if (m_socket != -1) {
        close(m_socket);
        m_socket = -1;
    }
}

esp_err_t ProtBackendUdp::start(uint16_t port) {
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == -1) {
        ESP_LOGE(RBPROT_TAG, "failed to create socket: %s", strerror(errno));
        return ESP_ERR_INVALID_STATE;
    }

    const int enable = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == -1) {
        ESP_LOGE(RBPROT_TAG, "failed to set SO_REUSEADDR: %s", strerror(errno));
        close(m_socket);
        m_socket = -1;
        return ESP_ERR_INVALID_STATE;
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
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

void ProtBackendUdp::send_from_queue(const QueueItem& it) {
    struct sockaddr_in send_addr = {
        .sin_len = sizeof(struct sockaddr_in),
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr = { 0 },
        .sin_zero = { 0 },
    };

    send_addr.sin_port = it.addr.udp.port;
    send_addr.sin_addr = it.addr.udp.ip;

    int res = ::sendto(m_socket, it.buf, it.size, 0, (struct sockaddr*)&send_addr, sizeof(struct sockaddr_in));
    if (res < 0) {
        ESP_LOGE(RBPROT_TAG, "error in sendto: %d %s!", errno, strerror(errno));
    }
}

void ProtBackendUdp::resend_mustarrive(const ProtocolAddr& addr, const rbjson::Object* pkt) {
    struct sockaddr_in send_addr = {
        .sin_len = sizeof(struct sockaddr_in),
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr = { 0 },
        .sin_zero = { 0 },
    };
    send_addr.sin_port = addr.udp.port;
    send_addr.sin_addr = addr.udp.ip;

    const auto str = pkt->str();
    int res = ::sendto(m_socket, str.c_str(), str.size(), 0, (struct sockaddr*)&send_addr, sizeof(struct sockaddr_in));
    if (res < 0) {
        ESP_LOGE(RBPROT_TAG, "error in sendto: %d %s!", errno, strerror(errno));
    }
}

std::unique_ptr<rbjson::Object> ProtBackendUdp::recv_iter(std::vector<uint8_t>& buf, ProtocolAddr& out_received_addr) {
    ssize_t received_len = 0;
    while (true) {
        received_len = recvfrom(m_socket, buf.data(), buf.size(), MSG_PEEK | MSG_DONTWAIT, NULL, NULL);
        if (received_len < 0) {
            const auto err = errno;
            if(err != EAGAIN) { // with MSG_DONTWAIT, it means no message available
                ESP_LOGE(RBPROT_TAG, "error in recvfrom: %d %s!", err, strerror(err));
            }
            return nullptr;
        }

        if (received_len < buf.size())
            break;
        buf.resize(buf.size() + 16);
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);

    const auto pop_res = recvfrom(m_socket, buf.data(), 0, 0, (struct sockaddr*)&addr, &addr_len);
    if (pop_res < 0) {
        const auto err = errno;
        ESP_LOGE(RBPROT_TAG, "error in recvfrom: %d %s!", err, strerror(err));
        return nullptr;
    }

    std::unique_ptr<rbjson::Object> pkt(rbjson::parse((char*)buf.data(), received_len));
    if (!pkt) {
        ESP_LOGE(RBPROT_TAG, "failed to parse the packet's json");
        return nullptr;
    }
    
    out_received_addr.kind = ProtBackendType::PROT_UDP;
    out_received_addr.udp.port = addr.sin_port;
    out_received_addr.udp.ip = addr.sin_addr;
    return pkt;
}

};
};

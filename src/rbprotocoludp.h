#pragma once

#include "rbprotocol.h"

namespace rb {
namespace internal {


class ProtBackendUdp {
public:
    ProtBackendUdp();
    ~ProtBackendUdp();

    esp_err_t start(uint16_t port);

    void send_from_queue(const QueueItem& it);
    void resend_mustarrive(const ProtocolAddr& addr, const rbjson::Object* pkt);

    std::unique_ptr<rbjson::Object> recv_iter(std::vector<uint8_t>& buf, ProtocolAddr& out_received_addr);

private:
    int m_socket;
};

};
};

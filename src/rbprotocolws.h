#pragma once

#include "rbprotocol.h"

namespace rb {
namespace internal {

class ProtBackendWs {
public:
    ProtBackendWs();
    ~ProtBackendWs();

    esp_err_t start(bool register_with_webserver);

    void send_from_queue(const QueueItem& it);

    std::unique_ptr<rbjson::Object> recv_iter(std::vector<uint8_t>& buf, ProtocolAddr& out_received_addr);

    void addClient(int fd);

private:
    enum ClientState : uint8_t {
        INITIAL,
        LEN1,
        MASK,
        DATA,
        FULLY_RECEIVED,
    };

    struct Client {
        Client(int fd) {
            this->fd = fd;
            remaining_payload_len = 0;
            flags = 0;
            state = ClientState::INITIAL;
        }

        bool fin() const { return flags >> 7; }
        uint8_t opcode() const { return flags & 0xF; }

        std::vector<uint8_t> payload;
        uint8_t masking_key[4];
        uint16_t remaining_payload_len;
        int fd;
        uint8_t flags;
        ClientState state;
    };

    int process_client(Client& client, std::vector<uint8_t>& buf);
    int process_client_header(Client& client, std::vector<uint8_t>& buf);
    std::unique_ptr<rbjson::Object> process_client_fully_received_locked(Client& client, ProtocolAddr& out_received_addr);

    void close_client(int fd);
    void close_client_locked(int fd);
    void close_client_locked_gracefully(int fd);

    std::vector<std::unique_ptr<Client>> m_clients;
    std::mutex m_clients_mu;
};

};
};

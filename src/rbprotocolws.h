#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <mutex>
#include <stdarg.h>
#include <string>
#include <vector>
#include <memory>

#include "rbjson.h"
#include "rbprotocolbase.h"

namespace rb {

struct WsAddr {
    int fd;
};

/**
 * \brief Class that manages the RBProtocol communication
 */
class ProtocolWs : public ProtocolImplBase<WsAddr> {
public:
    typedef ProtocolImplBase<WsAddr>::callback_t callback_t;

    /**
     * The onPacketReceivedCallback is called when a packet arrives.
     * It runs on a separate task, only single packet is processed at a time.
     */
    ProtocolWs(const char* owner, const char* name, const char* description, callback_t callback = nullptr);
    ~ProtocolWs() {}

    void start();
    void stop();

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

    static void send_task_trampoline(void* ctrl);
    void send_task();
    void resend_mustarrive_locked();

    static void recv_task_trampoline(void* ctrl);
    void recv_task();

    int process_client(Client &client, std::vector<uint8_t>& buf);
    int process_client_header(Client &client, std::vector<uint8_t>& buf);
    void process_client_fully_received_locked(Client &client);

    void close_client(int fd);

    bool is_addr_empty(const WsAddr& addr) const {
        return addr.fd == 0;
    }

    bool is_addr_same(const WsAddr& a, const WsAddr& b) const {
        return a.fd == b.fd;
    }

    TaskHandle_t m_task_send;
    TaskHandle_t m_task_recv;

    std::vector<std::unique_ptr<Client>> m_client_fd;
    std::mutex m_client_fd_mu;
};

};

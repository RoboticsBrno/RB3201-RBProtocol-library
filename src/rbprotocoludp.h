#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <mutex>
#include <stdarg.h>
#include <string>
#include <vector>

#include <lwip/arch.h>
#include <lwip/sockets.h>

#include "rbjson.h"
#include "rbprotocolbase.h"

namespace rb {

#define RBPROTOCOL_PORT 42424 //!< The default RBProtocol port

struct UdpSockAddr {
    struct in_addr ip;
    uint16_t port;
};

/**
 * \brief Class that manages the RBProtocol communication
 */
class ProtocolUdp : public ProtocolImplBase<UdpSockAddr> {
public:
    typedef ProtocolImplBase<UdpSockAddr>::callback_t callback_t;

    /**
     * The onPacketReceivedCallback is called when a packet arrives.
     * It runs on a separate task, only single packet is processed at a time.
     */
    ProtocolUdp(const char* owner, const char* name, const char* description, callback_t callback = nullptr);
    ~ProtocolUdp() {}

    void start(uint16_t port = RBPROTOCOL_PORT); //!< Start listening for UDP packets on port
    void stop(); //!< Stop listening

private:
    static void send_task_trampoline(void* ctrl);
    void send_task();
    void resend_mustarrive_locked();

    static void recv_task_trampoline(void* ctrl);
    void recv_task();

    bool is_addr_empty(const UdpSockAddr& addr) const {
        return addr.port == 0;
    }

    bool is_addr_same(const UdpSockAddr& a, const UdpSockAddr& b) const {
        return a.ip.s_addr == b.ip.s_addr && a.port == b.port;
    }

    int m_socket;
};

};

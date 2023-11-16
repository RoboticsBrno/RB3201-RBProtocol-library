#pragma once


#ifdef __cplusplus
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>
#include <vector>

struct sockaddr_in;

namespace rb {

class DnsServer {
public:
    static DnsServer& get();

    void start(const char *local_hostname = "esp32.local");
    void stop();

    const std::string& getLocalHostname() const {
        return m_local_hostname;
    }

private:
    DnsServer();
    DnsServer(const DnsServer&) = delete;
    ~DnsServer();

    ssize_t receivePacket(std::vector<uint8_t>& buf, struct sockaddr_in *addr);
    ssize_t processDnsQuestion(std::vector<uint8_t>& buf, ssize_t req_size);
    uint8_t *parseDnsName(uint8_t *buf, size_t maxlen, std::string& out_name);

    static void taskBody(void*);

    std::mutex m_mutex;
    std::string m_local_hostname;
    int m_socket;
    TaskHandle_t m_task;
};

};

#else

const char *rb_dn_get_local_hostname();

#endif // __cplusplus

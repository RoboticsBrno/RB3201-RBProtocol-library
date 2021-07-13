#pragma once

#include <atomic>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0)
#include <esp_event.h>
#else
#include <esp_event_loop.h>
#endif

#include <lwip/ip4_addr.h>

namespace rb {

class WiFiInitializer;
/**
 * \brief Helper class for connecting to the wifi
 */
class WiFi {
    friend class WiFiInitializer;

public:
    //!< Connect to a wifi network with given ssid (name) and password
    static void connect(const char* ssid, const char* password);

    //!< Create a wifi network with given ssid (name) and password
    static void startAp(const char* ssid, const char* password, uint8_t channel = 6);

    //!< Return current IP address of the ESP32
    static uint32_t getIp() { return m_ip.load(); }

private:
    static void init();

    static esp_err_t eventHandler_tcpip(void* ctx, system_event_t* event);
    static void eventHandler_netif(void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data);

    static std::atomic<uint32_t> m_ip;
};
};

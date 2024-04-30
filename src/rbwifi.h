#pragma once

#include <atomic>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>

#include "esp_wifi.h"

#if defined(ESP_IDF_VERSION_VAL)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0)
#define RBPROTOCOL_USE_NETIF 1
#include <esp_event.h>
#else
#include <esp_event_loop.h>
#endif
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
    // Results are sent to queue results. They are of type wifi_ap_record_t.
    //
    // Scan done can be awaited using the semaphore returned in out_scan_done_event.
    // Use normal WiFi APIs, eg. esp_wifi_scan_get_ap_record to get the results.
    // Example:
    //     SemaphoreHandle_t wifiScanDone;
    //     ESP_ERROR_CHECK(rb::WiFi::scanAsync(&wifiScanDone));
    //     
    //     xSemaphoreTake(wifiScanDone, portMAX_DELAY);
    //     
    //     uint16_t num;
    //     ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&num));
    //     
    //     printf("Found %d networks:\n", num);
    //     
    //     wifi_ap_record_t record;
    //     while(esp_wifi_scan_get_ap_record(&record) == ESP_OK) {
    //         printf("  %.32s #%d rssi %d auth %d\n",
    //             record.ssid, record.primary, record.rssi, record.authmode);
    //     }
    static esp_err_t scanAsync(SemaphoreHandle_t *out_scan_done_event);

    static std::vector<wifi_ap_record_t> scanSync(esp_err_t *err_out = NULL);

    //!< Connect to a wifi network with given ssid (name) and password
    static void connect(const char* ssid, const char* password);

    //!< Create a wifi network with given ssid (name) and password
    static void startAp(const char* ssid, const char* password, uint8_t channel = 6);

    //!< Return current IP address of the ESP32
    static uint32_t getIp() { return m_ip.load(); }

    static std::string getIpStr() {
        const auto ip = getIp();
        char buf[20];
        snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu", (ip >> 0) & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        return std::string(buf);
    }

    //!< Blocks until IP is assigned
    static bool waitForIp(TickType_t maxDelay = portMAX_DELAY) {
        const TickType_t step = pdMS_TO_TICKS(50);
        while (getIp() == 0 && maxDelay >= step) {
            vTaskDelay(step);
            maxDelay -= step;
        }
        return getIp() != 0;
    }

private:
    static void init();

#ifndef RBPROTOCOL_USE_NETIF
    static esp_err_t eventHandler_tcpip(void* ctx, system_event_t* event);
#else
    static void eventHandler_netif_wifi(void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data);
        static void eventHandler_netif_ip(void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data);
#endif
    static std::atomic<uint32_t> m_ip;
};
};

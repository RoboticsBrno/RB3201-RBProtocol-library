#include "esp_wifi.h"
#include "rbwifi.h"

// This is implementation for IDF >= 4.1
#ifdef RBPROTOCOL_USE_NETIF

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include <string.h>

#define TAG "RbWifi"

static esp_netif_t *gNetIf = nullptr;
static SemaphoreHandle_t gScanDoneEv = nullptr;

namespace rb {

class WiFiInitializer {
    friend class WiFi;

public:
    WiFiInitializer() {
        //ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_netif_init());

        auto err = esp_event_loop_create_default();
        if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &WiFi::eventHandler_netif_wifi,
            NULL,
            &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &WiFi::eventHandler_netif_ip,
            NULL,
            &instance_got_ip));

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = 0;
        cfg.nano_enable = 1;
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    }

    ~WiFiInitializer() {
    }
};

std::atomic<uint32_t> WiFi::m_ip;

void WiFi::init() {
    static WiFiInitializer init;
}

void WiFi::connect(const char* ssid, const char* pass) {
    init();

    esp_wifi_stop();

    if (gNetIf) {
        esp_netif_destroy_default_wifi(gNetIf);
    }

    gNetIf = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t cfg = {};
    snprintf((char*)cfg.sta.ssid, 32, "%s", ssid);
    snprintf((char*)cfg.sta.password, 64, "%s", pass);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    ESP_ERROR_CHECK(esp_wifi_start());
}

void WiFi::startAp(const char* ssid, const char* pass, uint8_t channel) {
    init();

    esp_wifi_stop();

    if (gNetIf) {
        esp_netif_destroy_default_wifi(gNetIf);
    }

    gNetIf = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t cfg = {};

    size_t pass_len = strlen(pass);
    if (pass_len < 8) {
        ESP_LOGE(TAG, "The WiFi password is too short, 8 characters required, leaving the WiFI open!");
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        if(pass_len >= 64) {
            ESP_LOGE(TAG, "The WiFi password is too long, using first 63 characters only.");
            pass_len = 63;
        }
        memcpy(cfg.ap.password, pass, pass_len);
        cfg.ap.password[pass_len] = 0;
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    size_t ssid_len = strlen(ssid);
    if(ssid_len >= 32) {
        ESP_LOGE(TAG, "The WiFi SSID password is too long, using first 31 characters only.");
        ssid_len = 31;
    }
    memcpy(cfg.ap.ssid, ssid, ssid_len);
    cfg.ap.ssid[ssid_len] = 0;

    cfg.ap.channel = channel;
    cfg.ap.beacon_interval = 400;
    cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));

    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);

    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(gNetIf, &ip_info));
    m_ip.store(ip_info.ip.addr);
}

esp_err_t WiFi::scanAsync(SemaphoreHandle_t *out_scan_done_event) {
    init();

    if(!out_scan_done_event) {
        return ESP_ERR_INVALID_ARG;
    }

    if(!gScanDoneEv) {
        gScanDoneEv = xSemaphoreCreateBinary();
    }
    *out_scan_done_event = gScanDoneEv;

    auto err = esp_wifi_scan_start(NULL, false);
    if(err == ESP_ERR_WIFI_NOT_STARTED) {
        WiFi::connect("", ""); // just start WiFi in station mode, with no target
        err = esp_wifi_scan_start(NULL, false);
    }
    
    if(err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

std::vector<wifi_ap_record_t> WiFi::scanSync(esp_err_t *err_out) {
    SemaphoreHandle_t done_ev;
    auto err = WiFi::scanAsync(&done_ev);
    if(err_out) {
        *err_out = err;
    }

    if(err != ESP_OK) {
        return {};
    }

    xSemaphoreTake(done_ev, pdMS_TO_TICKS(15000));

    uint16_t num = 0;
    err = esp_wifi_scan_get_ap_num(&num);
    if(err != ESP_OK) {
        if(err_out) {
            *err_out = err;
        }
        return {};
    }

    std::vector<wifi_ap_record_t> results(num);
    err = esp_wifi_scan_get_ap_records(&num, results.data());
    if(err_out) {
        *err_out = err;
    }
    return results;
}

void WiFi::eventHandler_netif_wifi(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data) {

    switch(event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGD(TAG, "SYSTEM_EVENT_STA_START");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            m_ip.store(0);
            ESP_LOGI(TAG, "SYSTEM_EVENT_STA_DISCONNECTED");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_SCAN_DONE: {
            if(gScanDoneEv) {
                xSemaphoreGive(gScanDoneEv);
            }
            break;
        }
    }
}

void WiFi::eventHandler_netif_ip(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data) {

    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;

        char buf[16];
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP");
        ESP_LOGI(TAG, "Got IP: %s\n",
            esp_ip4addr_ntoa(&event->ip_info.ip, buf, sizeof(buf)));
        m_ip.store(event->ip_info.ip.addr);
    }
}

}; // namespace rb

#endif

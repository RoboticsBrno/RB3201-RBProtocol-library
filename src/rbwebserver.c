#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <esp_http_server.h>

#define TAG "RbWebServer"

#define WORKING_DIRECTORY "/spiffs"

static esp_err_t static_file_handler(httpd_req_t *req) {
    char buf[256] = { 0 };

    if(snprintf(buf, sizeof(buf), "%s%s", WORKING_DIRECTORY, req->uri) >= sizeof(buf)) {
        ESP_LOGE(TAG, "the uri %s is too long!", req->uri);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int fd = open(buf, O_RDONLY);
    if(fd <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    int n = 0;
    esp_err_t err = ESP_OK;
    while((n = read(fd, buf, sizeof(buf))) > 0) {
        if((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK)
            goto exit;
    }
    err = httpd_resp_send_chunk(req, buf, 0);

exit:
    close(fd);
    return err;
}

static void add_handlers(httpd_handle_t server, const char *path) {
    DIR *d = opendir(path);
    if(d == NULL) {
        ESP_LOGE(TAG, "failed to open %s\n", path);
        return;
    }

    char buf[256] = { 0 };
    struct dirent *e = NULL;
    while((e = readdir(d)) != NULL) {
        if(snprintf(buf, sizeof(buf), "%s/%s", path, e->d_name) >= sizeof(buf)) {
            ESP_LOGE(TAG, "too long filename: %s/%s", path, e->d_name);
            continue;
        }

        switch(e->d_type) {
            case DT_DIR:
                add_handlers(server, buf);
                break;
            case DT_REG:
            {
                httpd_uri_t handler = {
                    .uri = buf+strlen(WORKING_DIRECTORY),
                    .method = HTTP_GET,
                    .handler = static_file_handler,
                };

                esp_err_t err = httpd_register_uri_handler(server, &handler);
                if(err != ESP_OK) {
                    ESP_LOGE(TAG, "failed to register uri handler for %s: %d\n", buf, err);
                }
                break;
            }
        }
    }

    closedir(d);
}

static void *new_web_task(void *portPtr) {
    int port = (int)portPtr;

    esp_vfs_spiffs_conf_t conf = {
      .base_path = WORKING_DIRECTORY,
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%d)", ret);
        }
        goto exit;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.task_priority = 3;
    config.max_uri_handlers = 32;

    esp_err_t err = httpd_start(&server, &config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start httpd server: %d", err);
        goto exit;
    }

    add_handlers(server, WORKING_DIRECTORY);

exit:
    vTaskDelete(NULL);
    return NULL;
}

TaskHandle_t rb_web_start(int port) {
    TaskHandle_t task;
    xTaskCreate(&new_web_task, "rbctrl_web", 3072, (void*)port, 3, &task);
    return task;
}

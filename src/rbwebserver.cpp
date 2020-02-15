#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <functional>

#include "esp_log.h"
#include "esp_spiffs.h"
#include <esp_http_server.h>

#define TAG "RbWebServer"

static const char *WORKING_DIRECTORY = "/spiffs";

typedef struct {
    const char *extension;
    const char *mime_type;
} mime_map;

static mime_map mime_types [] = {
    {".css", "text/css"},
    {".gif", "image/gif"},
    {".htm", "text/html"},
    {".html", "text/html"},
    {".jpeg", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".ico", "image/x-icon"},
    {".js", "application/javascript"},
    {".pdf", "application/pdf"},
    {".mp4", "video/mp4"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".xml", "text/xml"},
    {NULL, NULL},
};

static const char *default_mime_type = "text/plain";

static const char *get_mime_type(const char *path) {
    const int path_len = strlen(path);
    for(int i = 0; mime_types[i].extension != NULL; ++i) {
        const char *match = path + path_len - strlen(mime_types[i].extension);
        if(strstr(path, mime_types[i].extension) == match) {
            return mime_types[i].mime_type;
        }
    }
    return default_mime_type;
}

static esp_err_t static_file_handler(httpd_req_t *req) {
    char length_buf[32] = { 0 };
    char buf[512] = { 0 };

    if(snprintf(buf, sizeof(buf), "%s%s", WORKING_DIRECTORY, req->uri) >= sizeof(buf)) {
        ESP_LOGE(TAG, "the uri %s is too long!", req->uri);
        return ESP_ERR_NOT_SUPPORTED;
    }

    struct stat info;
    if(stat(buf, &info) < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    int fd = open(buf, O_RDONLY);
    if(fd <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    httpd_resp_set_type(req, get_mime_type(buf));

    if(snprintf(length_buf, sizeof(length_buf), "%ld", (long)info.st_size) < sizeof(buf)) {
        httpd_resp_set_hdr(req, "Content-Length", length_buf);
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

static esp_err_t walk_dir_files(const char *path, std::function<esp_err_t(const char *path)> callback) {
    DIR *d = opendir(path);
    if(d == NULL) {
        ESP_LOGE(TAG, "failed to open %s\n", path);
        return ESP_FAIL;
    }

    char buf[256] = { 0 };
    struct dirent *e = NULL;
    esp_err_t err;
    while((e = readdir(d)) != NULL) {
        if(snprintf(buf, sizeof(buf), "%s/%s", path, e->d_name) >= sizeof(buf)) {
            ESP_LOGE(TAG, "too long filename: %s/%s", path, e->d_name);
            continue;
        }

        switch(e->d_type) {
            case DT_DIR:
                if((err = walk_dir_files(buf, callback)) != ESP_OK)
                    return err;
                break;
            case DT_REG:
                if((err = callback(buf)) != ESP_OK)
                    return err;
                break;
        }
    }

    closedir(d);
    return ESP_OK;
}

esp_err_t rb_web_start(int port) {
    esp_vfs_spiffs_conf_t conf = {
      .base_path = WORKING_DIRECTORY,
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%d)", err);
        }
        return err;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.task_priority = 3;
    config.max_uri_handlers = 0;

    err = walk_dir_files(WORKING_DIRECTORY, [&](const char *) -> esp_err_t {
        ++config.max_uri_handlers;
        return ESP_OK;
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to list files in SPIFFS: %d", err);
        return err;
    }

    err = httpd_start(&server, &config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start httpd server: %d", err);
        return err;
    }

    return walk_dir_files(WORKING_DIRECTORY, [&](const char *path) -> esp_err_t {
        httpd_uri_t handler = {
            .uri = path+strlen(WORKING_DIRECTORY),
            .method = HTTP_GET,
            .handler = static_file_handler,
        };

        esp_err_t err = httpd_register_uri_handler(server, &handler);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "failed to register uri handler for %s: %d\n", path, err);
            return err;
        }
        return ESP_OK;
    });
}

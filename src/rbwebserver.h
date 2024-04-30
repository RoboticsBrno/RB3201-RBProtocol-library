#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Start serving files from SPIFFS on http on port.
 */
TaskHandle_t rb_web_start(int port);

/**
 * \brief Stop http server. Pass in the task handle returned by rb_web_start.
 */
void rb_web_stop(TaskHandle_t web_task);

/**
 * \brief Adds another file into the web server's root.
 */
esp_err_t rb_web_add_file(const char* filename, const char* data, size_t len);

/**
 * \brief Returns path to root of files served by the web server. Without trailing /.
 */
const char *rb_web_get_files_root(void);

/**
 * Set a callback to call whenever URL in directory /extra/ is accessed.
*/
void rb_web_set_extra_callback(void (*callback)(const char* request_path, int out_fd));

#ifdef __cplusplus
};
#endif

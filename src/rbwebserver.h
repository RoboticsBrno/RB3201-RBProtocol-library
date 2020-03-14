#pragma once

#include "FreeRTOS.h"

#include <esp_err.h>

/**
 * \brief Start serving files from SPIFFS on http on port.
 */
extern "C" TaskHandle_t rb_web_start(int port);

/**
 * \brief Adds another file into the web server's root.
 */
extern "C" esp_err_t rb_web_add_file(const char *filename, const char *data, size_t len);

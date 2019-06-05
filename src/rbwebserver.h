#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Start serving files from SPIFFS on http on port.
 */
TaskHandle_t rb_web_start(int port);

#ifdef __cplusplus
}
#endif
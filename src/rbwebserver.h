#pragma once

/**
 * \brief Start serving files from SPIFFS on http on port.
 */
extern "C" TaskHandle_t rb_web_start(int port);

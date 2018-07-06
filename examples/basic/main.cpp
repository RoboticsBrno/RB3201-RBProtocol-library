#include <esp_log.h>
#include <lwip/sockets.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>

#include "rbprotocol.h"
#include "rbwebserver.h"

void onPktReceived(void *cookie, const std::string& command, rbjson::Object *pkt) {
    if(command == "joy") {
        printf("Joy: ");
        rbjson::Array *data = pkt->getArray("data");
        for(size_t i = 0; i < data->size(); ++i) {
            rbjson::Object *ax = data->getObject(i);
            printf("#%d %6lld %6lld | ", i, ax->getInt("x"), ax->getInt("y")); 
        }
        printf("\r");
    } else if(command == "fire") {
        printf("\n\nFIRE THE MISSILESS\n\n");
    }
}

extern "C" void app_main() {
    // Create web server, serves static files from the spiffs memory
    rb_web_start(80);
    
    RbProtocol rb("Foo", "Bar", "The very best bar", &onPktReceived);
    rb.start();

    printf("Hello world!\n");

    int i = 0;
    while(true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        if(rb.is_possessed()) {
            rb.send_log("Tick #%d\n", i++);
        }
    }
}
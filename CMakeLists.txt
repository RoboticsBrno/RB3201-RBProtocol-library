cmake_minimum_required(VERSION 3.5)

set(SRCS
    "src/rbdns.cpp"
    "src/rbjson.cpp"
    "src/rbprotocol.cpp"
    "src/rbprotocoludp.cpp"
    "src/rbprotocolws.cpp"
    "src/rbtcp.cpp"
    "src/rbudp.cpp"
    "src/rbwifi_netif.cpp"
    "src/rbwifi_tcpip.cpp"
    "src/mpaland-printf/printf.c"
    "src/rbwebserver.c"
    "src/rbwebsockets.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "./src"
    REQUIRES nvs_flash lwip spiffs esp_netif esp_event esp_wifi mbedtls
)

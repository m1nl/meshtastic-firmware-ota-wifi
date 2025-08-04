set(SOURCES
    "main.c"
    "otaserver.c"
)

set(PRIV_REQUIRES
    nvs_flash
    esp_wifi
    app_update
    esp_http_server
    spi_flash
)

idf_component_register(SRCS ${SOURCES} INCLUDE_DIRS "." PRIV_REQUIRES ${PRIV_REQUIRES})

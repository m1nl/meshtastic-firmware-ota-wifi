set(SOURCES
    "main.c"
)

set(PRIV_REQUIRES
    nvs_flash
    esp_wifi
    mbedtls
    app_update
)

idf_component_register(SRCS ${SOURCES} INCLUDE_DIRS "." PRIV_REQUIRES ${PRIV_REQUIRES})

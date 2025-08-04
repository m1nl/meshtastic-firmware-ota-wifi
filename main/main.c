#include <string.h>

#include <nvs_flash.h>

#include <esp_log.h>
#include <esp_wifi.h>

#include <mdns.h>

#include "esp_image_format.h"
#include "esp_ota_ops.h"

#include "otaserver.h"

#define TAG "OTA"
#define INFO(format, ...)                                                                                             \
    do {                                                                                                              \
        ESP_LOGI(TAG, format, ##__VA_ARGS__);                                                                         \
    } while (0)
#define WARN(format, ...)                                                                                             \
    do {                                                                                                              \
        ESP_LOGW(TAG, format, ##__VA_ARGS__);                                                                         \
    } while (0)
#define FAIL(format, ...)                                                                                             \
    do {                                                                                                              \
        ESP_LOGE(TAG, format, ##__VA_ARGS__);                                                                         \
        esp_restart();                                                                                                \
    } while (0)

#define HOSTNAME "meshtastic-ota"
#define MDNS_INSTANCE "Meshtastic OTA Web server"

typedef struct {
    char ssid[32];
    char psk[64];
} wifi_credentials_t;

static nvs_handle_t s_nvs_handle;

static void nvs_init(const char *namespace) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open(namespace, NVS_READWRITE, &s_nvs_handle));
}

static void nvs_read_config(wifi_credentials_t *config) {
    size_t ssid_len = sizeof(config->ssid);
    size_t psk_len = sizeof(config->psk);

    ESP_ERROR_CHECK(nvs_get_str(s_nvs_handle, "ssid", config->ssid, &ssid_len));
    ESP_ERROR_CHECK(nvs_get_str(s_nvs_handle, "psk", config->psk, &psk_len));
    ESP_ERROR_CHECK(nvs_set_u8(s_nvs_handle, "updated", 0));
    ESP_ERROR_CHECK(nvs_commit(s_nvs_handle));
}

static void nvs_mark_updated() {
    ESP_ERROR_CHECK(nvs_set_u8(s_nvs_handle, "updated", 1));
    ESP_ERROR_CHECK(nvs_commit(s_nvs_handle));
    nvs_close(s_nvs_handle);
}

static const int wifi_connect_retries = 10;
static const EventBits_t BIT_CONNECTED = BIT0;
static const EventBits_t BIT_FAIL = BIT1;

static EventGroupHandle_t event_group_handle;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    static int s_retry_num = 0;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            INFO("WiFi connect");
            esp_wifi_connect();

        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry_num < wifi_connect_retries) {
                INFO("WiFi connect retry");
                esp_wifi_connect();
                ++s_retry_num;

            } else {
                xEventGroupSetBits(event_group_handle, BIT_FAIL);
            }
        }

    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            INFO("WiFi got IP");
            s_retry_num = 0;
            xEventGroupSetBits(event_group_handle, BIT_CONNECTED);
        }

    } else {
        FAIL("Unknown event");
    }
}

static void wifi_connect(const wifi_credentials_t *config) {
    event_group_handle = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta.threshold.authmode = WIFI_AUTH_WPA_PSK,
    };
    strncpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, config->psk, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits =
        xEventGroupWaitBits(event_group_handle, BIT_CONNECTED | BIT_FAIL, pdFALSE, pdFALSE, portMAX_DELAY);

    if (!(bits & BIT_CONNECTED)) {
        FAIL("Failed to connect to WiFi AP");
    }
}

static void mdns_setup(void) {
    ESP_ERROR_CHECK(mdns_init());

    mdns_hostname_set(HOSTNAME);
    mdns_instance_name_set(MDNS_INSTANCE);

    static mdns_txt_item_t serviceTxtData[] = {{"board", "esp32"}, {"path", "/"}};

    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, serviceTxtData,
                                         sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

static const char *get_type_str(esp_partition_type_t type) {
    switch (type) {
        case ESP_PARTITION_TYPE_APP:
            return "app";
        case ESP_PARTITION_TYPE_DATA:
            return "data";
        default:
            return "unknown";
    }
}

static const char *get_subtype_str(esp_partition_type_t type, esp_partition_subtype_t subtype) {
    if (type == ESP_PARTITION_TYPE_APP) {
        switch (subtype) {
            case ESP_PARTITION_SUBTYPE_APP_FACTORY:
                return "factory";
            case ESP_PARTITION_SUBTYPE_APP_OTA_0:
                return "ota_0";
            case ESP_PARTITION_SUBTYPE_APP_OTA_1:
                return "ota_1";
            case ESP_PARTITION_SUBTYPE_APP_OTA_2:
                return "ota_2";
            case ESP_PARTITION_SUBTYPE_APP_OTA_3:
                return "ota_3";
            case ESP_PARTITION_SUBTYPE_APP_OTA_4:
                return "ota_4";
            case ESP_PARTITION_SUBTYPE_APP_OTA_5:
                return "ota_5";
            case ESP_PARTITION_SUBTYPE_APP_OTA_6:
                return "ota_6";
            case ESP_PARTITION_SUBTYPE_APP_OTA_7:
                return "ota_7";
            case ESP_PARTITION_SUBTYPE_APP_OTA_8:
                return "ota_8";
            case ESP_PARTITION_SUBTYPE_APP_OTA_9:
                return "ota_9";
            case ESP_PARTITION_SUBTYPE_APP_OTA_10:
                return "ota_10";
            case ESP_PARTITION_SUBTYPE_APP_OTA_11:
                return "ota_11";
            case ESP_PARTITION_SUBTYPE_APP_OTA_12:
                return "ota_12";
            case ESP_PARTITION_SUBTYPE_APP_OTA_13:
                return "ota_13";
            case ESP_PARTITION_SUBTYPE_APP_OTA_14:
                return "ota_14";
            case ESP_PARTITION_SUBTYPE_APP_OTA_15:
                return "ota_15";
            case ESP_PARTITION_SUBTYPE_APP_TEST:
                return "test";
            default:
                return "unknown";
        }
    } else if (type == ESP_PARTITION_TYPE_DATA) {
        switch (subtype) {
            case ESP_PARTITION_SUBTYPE_DATA_OTA:
                return "ota";
            case ESP_PARTITION_SUBTYPE_DATA_PHY:
                return "phy";
            case ESP_PARTITION_SUBTYPE_DATA_NVS:
                return "nvs";
            case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
                return "coredump";
            case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
                return "nvskeys";
            case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM:
                return "efuse";
            case ESP_PARTITION_SUBTYPE_DATA_UNDEFINED:
                return "undefined";
            case ESP_PARTITION_SUBTYPE_DATA_ESPHTTPD:
                return "esphttpd";
            case ESP_PARTITION_SUBTYPE_DATA_FAT:
                return "fat";
            case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
                return "spiffs";
            case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS:
                return "littlefs";
            default:
                return "unknown";
        }
    }

    return "unknown";
}

static void print_info(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    INFO("%s %s %s %s %s", desc->project_name, desc->version, desc->idf_ver, desc->date, desc->time);

    esp_partition_iterator_t part_it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (part_it) {
        const esp_partition_t *part = esp_partition_get(part_it);
        const char *type = get_type_str(part->type);
        const char *subtype = get_subtype_str(part->type, part->subtype);

        INFO("%16s %7s %9s 0x%08" PRIx32 " %10" PRIi32 " %5" PRIu32, part->label, type, subtype,
                 part->address, part->size, part->erase_size);
        part_it = esp_partition_next(part_it);
    }
}

static void otaserver_event_cb(uint8_t event) {
    switch (event) {
        case OTA_EVENT_SUCCESS:
            nvs_mark_updated();
            break;
    }
}

void app_main() {
    print_info();
    nvs_init("ota-wifi");

    wifi_credentials_t config;
    INFO("Reading NVRAM storage");
    nvs_read_config(&config);

    INFO("Connecting to WiFi AP \"%s\"", config.ssid);
    wifi_connect(&config);

    INFO("Setting hostname and mDNS");
    mdns_setup();

    INFO("Starting web server");
    ESP_ERROR_CHECK(otaserver_start(&otaserver_event_cb));
}

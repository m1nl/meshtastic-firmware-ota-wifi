#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#define OTA_BUFFSIZE 1024

#define OTA_RESTART_DELAY_MS (3000)
#define OTA_RESTART_DELAY_TICKS (pdMS_TO_TICKS(OTA_RESTART_DELAY_MS))

#define OTA_EVENT_IDLE 0
#define OTA_EVENT_BEGIN 1
#define OTA_EVENT_SUCCESS 2
#define OTA_EVENT_FAILED 3

#define HTTPD_202 "202 Accepted" /*!< HTTP Response 202 */

typedef void (*otaserver_event_cb_t)(uint8_t);

esp_err_t otaserver_start(otaserver_event_cb_t);
esp_err_t otaserver_stop(void);

#ifdef __cplusplus
}
#endif

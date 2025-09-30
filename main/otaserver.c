#include "otaserver.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <inttypes.h>
#include <sys/param.h>

#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "spi_flash_mmap.h"

#define TAG "otaserver"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
static esp_pm_lock_handle_t pm_handle;
#define PM_LOCK_ACQUIRE() PM_LOCK_ACQUIRE()
#define PM_LOCK_RELEASE() PM_LOCK_RELEASE()
#else
#define PM_LOCK_ACQUIRE()
#define PM_LOCK_RELEASE()
#endif

#define ARRAY_LEN(x) (sizeof(x) / sizeof(x[0]))

static httpd_handle_t otaserver;
static otaserver_event_cb_t otaserver_event_cb;

const char index_html[] =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "  <meta charset=\"UTF-8\">"
    "  <title>Firmware Update</title>"
    "</head>"
    "<body style=\"font-family: monospace\">"
    "  <h1>Firmware Update</h1>"
    "  <input type=\"file\" id=\"firmware\" accept=\".bin\"><br><br>"
    "  <button onclick=\"uploadFirmware()\">Upload firmware</button>"
    "  <button onclick=\"downloadCoredump()\">Download coredump</button>"
    "  <button onclick=\"rebootToApp()\">Reboot to app</button>"
    "  <hr>"
    "  <pre id=\"status\"></pre>"
    "  <script>"
    "    async function uploadFirmware() {"
    "      const fileInput = document.getElementById('firmware');"
    "      const status = document.getElementById('status');"
    "      if (!fileInput.files.length) {"
    "        status.textContent = 'No file selected.';"
    "        return;"
    "      }"
    "      const file = fileInput.files[0];"
    "      const data = await file.arrayBuffer();"
    "      try {"
    "        status.textContent = 'Uploading...';"
    "        const res = await fetch('/ota', {"
    "          method: 'POST',"
    "          headers: { 'Content-Type': 'application/octet-stream' },"
    "          body: data"
    "        });"
    "        status.textContent = res.ok ? 'Upload successful.' : 'Upload failed: ' + res.statusText;"
    "      } catch (err) {"
    "        status.textContent = 'Error: ' + err;"
    "      }"
    "    }"
    "    async function downloadCoredump() {"
    "      const status = document.getElementById('status');"
    "      try {"
    "        const res = await fetch('/coredump');"
    "        if (!res.ok) throw new Error('Failed to fetch coredump');"
    "        const blob = await res.blob();"
    "        const url = URL.createObjectURL(blob);"
    "        const a = document.createElement('a');"
    "        a.href = url;"
    "        a.download = 'coredump.bin';"
    "        a.click();"
    "        URL.revokeObjectURL(url);"
    "      } catch (err) {"
    "        status.textContent = 'Error: ' + err;"
    "      }"
    "    }"
    "    async function rebootToApp() {"
    "      const status = document.getElementById('status');"
    "      try {"
    "        const res = await fetch('/reboot', {"
    "          method: 'POST'"
    "        });"
    "        status.textContent = res.ok ? 'Reboot successful.' : 'Reboot failed: ' + res.statusText;"
    "      } catch (err) {"
    "        status.textContent = 'Error: ' + err;"
    "      }"
    "    }"
    "  </script>"
    "</body>"
    "</html>";

void esp_restart_task(void *pvParameter);

esp_err_t ota_post_handler(httpd_req_t *req) {
    esp_err_t err;
    bool image_header_was_checked;

    char ota_write_data[OTA_BUFFSIZE + 1];

    ssize_t data_read;
    size_t binary_file_length;

    PM_LOCK_ACQUIRE();

    if (otaserver_event_cb != NULL) {
        (*otaserver_event_cb)(OTA_EVENT_BEGIN);
    }

    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "starting OTA handler");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        ESP_LOGW(TAG,
                 "configured OTA boot partition at offset 0x%08" PRIx32 ", but running "
                 "from offset 0x%08" PRIx32,
                 configured->address, running->address);
        ESP_LOGW(TAG, "(this can happen if either the OTA boot data or preferred "
                      "boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "running partition type %d subtype %d (offset 0x%08" PRIx32 ")", running->type, running->subtype,
             running->address);

    update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    ESP_LOGI(TAG, "writing to partition subtype %d at offset 0x%08" PRIx32, update_partition->subtype,
             update_partition->address);
    assert(update_partition != NULL);

    image_header_was_checked = false;
    binary_file_length = 0;

    while (binary_file_length < req->content_len) {
        if (otaserver_event_cb != NULL) {
            (*otaserver_event_cb)(OTA_EVENT_IDLE);
        }

        data_read = httpd_req_recv(req, ota_write_data, MIN(req->content_len - binary_file_length, OTA_BUFFSIZE));

        if (data_read < 0) {
            if (data_read == HTTPD_SOCK_ERR_TIMEOUT) {
                // retry receiving if timeout occurred
                continue;
            }

            ESP_LOGE(TAG, "data read error");
            if (image_header_was_checked) {
                esp_ota_abort(update_handle);
            }

            httpd_resp_set_status(req, HTTPD_400);
            httpd_resp_send(req, NULL, 0);

            if (otaserver_event_cb != NULL) {
                (*otaserver_event_cb)(OTA_EVENT_FAILED);
            }

            PM_LOCK_RELEASE();
            return ESP_FAIL;

        } else if (data_read > 0) {
            if (!image_header_was_checked) {
                esp_app_desc_t new_app_info;

                if (data_read >
                    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                    memcpy(&new_app_info,
                           &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)],
                           sizeof(esp_app_desc_t));

                    ESP_LOGI(TAG, "got chunk of size %d, parsing header", data_read);

                    ESP_LOGI(TAG, "new firmware version: %s", new_app_info.version);

                    esp_app_desc_t running_app_info;
                    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                        ESP_LOGI(TAG, "running firmware version: %s", running_app_info.version);
                    }

                    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));

                        httpd_resp_set_status(req, HTTPD_400);
                        httpd_resp_send(req, NULL, 0);

                        if (otaserver_event_cb != NULL) {
                            (*otaserver_event_cb)(OTA_EVENT_FAILED);
                        }

                        PM_LOCK_RELEASE();
                        return ESP_FAIL;
                    }

                    image_header_was_checked = true;

                    ESP_LOGI(TAG, "esp_ota_begin succeeded");

                } else {
                    ESP_LOGE(TAG, "received package does not fit header length");
                    esp_ota_abort(update_handle);

                    httpd_resp_set_status(req, HTTPD_400);
                    httpd_resp_send(req, NULL, 0);

                    if (otaserver_event_cb != NULL) {
                        (*otaserver_event_cb)(OTA_EVENT_FAILED);
                    }

                    PM_LOCK_RELEASE();
                    return ESP_FAIL;
                }
            }

            err = esp_ota_write(update_handle, (const void *)ota_write_data, data_read);

            if (err != ESP_OK) {
                esp_ota_abort(update_handle);

                httpd_resp_set_status(req, HTTPD_500);
                httpd_resp_send(req, NULL, 0);

                if (otaserver_event_cb != NULL) {
                    (*otaserver_event_cb)(OTA_EVENT_FAILED);
                }

                PM_LOCK_RELEASE();
                return ESP_FAIL;
            }

            binary_file_length += data_read;
            ESP_LOGD(TAG, "written image length %d", binary_file_length);

        } else if (data_read == 0) {
            ESP_LOGE(TAG, "connection closed");
            if (image_header_was_checked) {
                esp_ota_abort(update_handle);
            }

            httpd_resp_set_status(req, HTTPD_400);
            httpd_resp_send(req, NULL, 0);

            if (otaserver_event_cb != NULL) {
                (*otaserver_event_cb)(OTA_EVENT_FAILED);
            }

            PM_LOCK_RELEASE();
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "total write binary data length: %d", binary_file_length);

    if (otaserver_event_cb != NULL) {
        (*otaserver_event_cb)(OTA_EVENT_IDLE);
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));

        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_send(req, NULL, 0);

        if (otaserver_event_cb != NULL) {
            (*otaserver_event_cb)(OTA_EVENT_FAILED);
        }

        PM_LOCK_RELEASE();
        return ESP_FAIL;
    }

    if (otaserver_event_cb != NULL) {
        (*otaserver_event_cb)(OTA_EVENT_IDLE);
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));

        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_send(req, NULL, 0);

        if (otaserver_event_cb != NULL) {
            (*otaserver_event_cb)(OTA_EVENT_FAILED);
        }

        PM_LOCK_RELEASE();
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, HTTPD_202);
    httpd_resp_send(req, NULL, 0);

    if (otaserver_event_cb != NULL) {
        (*otaserver_event_cb)(OTA_EVENT_SUCCESS);
    }

    ESP_LOGI(TAG, "prepare to system restart");
    xTaskCreate(esp_restart_task, "esp_restart_task", 1024, NULL, 5, NULL);

    PM_LOCK_RELEASE();

    return ESP_OK;
}

esp_err_t reboot_post_handler(httpd_req_t *req) {
    esp_err_t err;
    const esp_partition_t *update_partition = NULL;

    PM_LOCK_ACQUIRE();

    update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    assert(update_partition != NULL);

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));

        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_send(req, NULL, 0);

        if (otaserver_event_cb != NULL) {
            (*otaserver_event_cb)(OTA_EVENT_FAILED);
        }

        PM_LOCK_RELEASE();
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, HTTPD_202);
    httpd_resp_send(req, NULL, 0);

    if (otaserver_event_cb != NULL) {
        (*otaserver_event_cb)(OTA_EVENT_REBOOT);
    }

    ESP_LOGI(TAG, "prepare to system restart");
    xTaskCreate(esp_restart_task, "esp_restart_task", 1024, NULL, 5, NULL);

    PM_LOCK_RELEASE();

    return ESP_OK;
}

esp_err_t index_get_handler(httpd_req_t *req) {
    PM_LOCK_ACQUIRE();

    if (otaserver_event_cb != NULL) {
        (*otaserver_event_cb)(OTA_EVENT_IDLE);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, index_html);

    PM_LOCK_RELEASE();

    return ESP_OK;
}

esp_err_t coredump_get_handler(httpd_req_t *req) {
    esp_err_t err;

    size_t data_read;
    ssize_t chunk_size;

    char coredump_read_data[OTA_BUFFSIZE + 1];

    const void *map_ptr;
    spi_flash_mmap_handle_t map_handle;

    PM_LOCK_ACQUIRE();

    if (otaserver_event_cb != NULL) {
        (*otaserver_event_cb)(OTA_EVENT_IDLE);
    }

    ESP_LOGI(TAG, "starting coredump handler");

    // find the partition map in the partition table
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "coredump");

    if (partition == NULL) {
        ESP_LOGE(TAG, "coredump partition not found");

        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_send(req, NULL, 0);

        PM_LOCK_RELEASE();
        return ESP_FAIL;
    }

    // map the partition to data memory
    err = esp_partition_mmap(partition, 0, partition->size, SPI_FLASH_MMAP_DATA, &map_ptr, &map_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unable to mmap coredump partition");

        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_send(req, NULL, 0);

        PM_LOCK_RELEASE();
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "application/octet-stream");

    // stream the partition content
    data_read = 0;
    while (data_read < partition->size) {
        chunk_size = partition->size - data_read;

        if (chunk_size > OTA_BUFFSIZE) {
            chunk_size = OTA_BUFFSIZE;
        }

        memcpy(coredump_read_data, map_ptr + data_read, chunk_size);
        err = httpd_resp_send_chunk(req, coredump_read_data, chunk_size);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "http write error");

            spi_flash_munmap(map_handle);
            PM_LOCK_RELEASE();
            return ESP_FAIL;
        }

        data_read += chunk_size;

        if (otaserver_event_cb != NULL) {
            (*otaserver_event_cb)(OTA_EVENT_IDLE);
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);

    spi_flash_munmap(map_handle);
    PM_LOCK_RELEASE();

    return ESP_OK;
}

static const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL};

static const httpd_uri_t index_html_uri = {
    .uri = "/index.html", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL};

static const httpd_uri_t index_htm_uri = {
    .uri = "/index.htm", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL};

static const httpd_uri_t ota_uri = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = NULL};

static const httpd_uri_t reboot_uri = {
    .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post_handler, .user_ctx = NULL};

static const httpd_uri_t coredump_uri = {
    .uri = "/coredump", .method = HTTP_GET, .handler = coredump_get_handler, .user_ctx = NULL};

static const httpd_uri_t *uri_handlers[] = {&root_uri, &index_html_uri, &index_htm_uri,
                                            &ota_uri,  &reboot_uri,     &coredump_uri};

void esp_restart_task(void *pvParameter) {
    vTaskDelay(OTA_RESTART_DELAY_TICKS);
    esp_restart();

    vTaskDelete(NULL);
}

esp_err_t otaserver_start(otaserver_event_cb_t event_cb) {
    esp_err_t err;
    uint8_t i;

#ifdef CONFIG_PM_ENABLE
    ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "otaserver-no-sleep-lock", &pm_handle);
    assert(ret == ESP_OK);
#endif

    PM_LOCK_ACQUIRE();

    otaserver_event_cb = event_cb;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.stack_size = 8 * 1024;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "starting server on port: '%d'", config.server_port);
    err = httpd_start(&otaserver, &config);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "error starting otaserver");

        PM_LOCK_RELEASE();
        return err;
    }

    ESP_LOGI(TAG, "registering URI handlers");

    for (i = 0; i < ARRAY_LEN(uri_handlers); i++) {
        err = httpd_register_uri_handler(otaserver, uri_handlers[i]);

        if (err != ESP_OK) {
            ESP_LOGI(TAG, "error registering URI handlers");

            httpd_stop(otaserver);
            PM_LOCK_RELEASE();
            return err;
        }
    }

    PM_LOCK_RELEASE();

    return ESP_OK;
}

esp_err_t otaserver_stop(void) { return httpd_stop(otaserver); }

// Stub esp_camera API for ESP32-P4
// TODO: Replace with esp_driver_cam (MIPI CSI) native implementation
#include <string.h>
#include "esp_camera.h"
#include "esp_log.h"

static const char *TAG = "cam_stub";

esp_err_t esp_camera_init(const camera_config_t *config) {
    ESP_LOGW(TAG, "esp_camera_init not implemented for ESP32-P4");
    ESP_LOGW(TAG, "Use esp_driver_cam for MIPI CSI camera support");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_camera_deinit(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

sensor_t *esp_camera_sensor_get(void) {
    static sensor_t stub_sensor;
    memset(&stub_sensor, 0, sizeof(stub_sensor));
    stub_sensor.status.framesize = FRAMESIZE_VGA;
    stub_sensor.pixformat = PIXFORMAT_JPEG;
    ESP_LOGW(TAG, "esp_camera_sensor_get returning stub");
    return &stub_sensor;
}

camera_fb_t *esp_camera_fb_get(void) {
    ESP_LOGW(TAG, "esp_camera_fb_get not implemented for ESP32-P4");
    return NULL;
}

void esp_camera_fb_return(camera_fb_t *fb) {
    // No-op stub
    (void)fb;
}

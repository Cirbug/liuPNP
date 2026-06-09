// Stub jpeg_decoder.h for P4 build (esp_jpeg component unavailable)
// Provides minimal definitions needed by esp_camera headers
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPEG_IMAGE_SCALE_0   = 0,
    JPEG_IMAGE_SCALE_1_2 = 1,
    JPEG_IMAGE_SCALE_1_4 = 2,
    JPEG_IMAGE_SCALE_1_8 = 3,
} esp_jpeg_image_scale_t;

typedef struct {
    // Stub - JPEG functionality not available for ESP32-P4 target
    int width;
    int height;
} jpeg_decoder_handle_t;

#ifdef __cplusplus
}
#endif

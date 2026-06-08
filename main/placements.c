/*
 * placements.c — CSV 坐标加载实现
 */

#include "placements.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "placements";

esp_err_t placements_load(placement_t *out_list, int max_count, int *out_count) {
    *out_count = 0;

    FILE *f = fopen(CSV_PATH_SDCARD, "r");
    if (!f) {
        ESP_LOGW(TAG, "%s not found", CSV_PATH_SDCARD);
        return ESP_ERR_NOT_FOUND;
    }

    char line[256];
    int idx = 0;

    /* 跳过表头 */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return ESP_FAIL;
    }

    while (fgets(line, sizeof(line), f) && idx < max_count) {
        /* 跳过空行 */
        if (line[0] == '\n' || line[0] == '\r') continue;

        placement_t *p = &out_list[idx];
        char layer[16];
        int feeder = 1;

        int n = sscanf(line,
            "%15[^,],%23[^,],%f,%f,%15[^,],%f,%d",
            p->designator, p->footprint,
            &p->x_mm, &p->y_mm,
            layer, &p->rotation, &feeder);

        if (n >= 5) {
            p->feeder = (n >= 7) ? feeder : 1;
            ESP_LOGI(TAG, "[%d] %s %s @ (%.2f,%.2f) R%.0f feeder=%d",
                     idx, p->designator, p->footprint,
                     p->x_mm, p->y_mm, p->rotation, p->feeder);
            idx++;
        } else if (n > 0) {
            ESP_LOGW(TAG, "Skip malformed: %s", line);
        }
    }

    fclose(f);
    *out_count = idx;
    ESP_LOGI(TAG, "Loaded %d placements", idx);
    return (idx > 0) ? ESP_OK : ESP_FAIL;
}

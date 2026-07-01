/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sentry_selftest.h"
#include "motion_detect.h"
#include <hal/board/hal_bridge.h>
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "jpg/image_to_jpeg.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <cstdio>
#include <sys/param.h>

static const char* TAG = "SentryVerify";

// Motion trigger: fraction of grid cells that changed, in permille.
static const int MOTION_TRIG_PERMILLE = 60;  // 6% of the frame changed

static void dump_jpeg_b64(const uint8_t* data, size_t sz, int w, int h, int fmt)
{
    uint8_t* jpg = nullptr;
    size_t jlen  = 0;
    if (image_to_jpeg((uint8_t*)data, sz, w, h, (v4l2_pix_fmt_t)fmt, 80, &jpg, &jlen) && jpg) {
        size_t cap         = 4 * ((jlen + 2) / 3) + 1;
        size_t olen        = 0;
        unsigned char* b64 = (unsigned char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (b64 && mbedtls_base64_encode(b64, cap, &olen, jpg, jlen) == 0) {
            mclog::tagInfo(TAG, "JPEG_B64_BEGIN jpeg_bytes={} b64_len={}", (int)jlen, (int)olen);
            for (size_t off = 0; off < olen; off += 100) {
                size_t n = MIN((size_t)100, olen - off);
                printf("B64:%.*s\n", (int)n, (char*)(b64 + off));
            }
            mclog::tagInfo(TAG, "JPEG_B64_END");
        }
        if (b64) heap_caps_free(b64);
        free(jpg);
    }
}

static void verify_task(void* /*arg*/)
{
    mclog::tagInfo(TAG, "===== MOTION VERIFY: static scene should read ~0; wave a hand to spike it =====");
    MotionDetector md;
    int64_t last_dump = -100000000;
    const int N       = 400;  // ~2-3 min window

    for (int i = 0; i < N; i++) {
        auto* cam = hal_bridge::board_get_camera();
        if (!cam || !cam->StreamCaptures()) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        const uint8_t* data = cam->GetFrameData();
        int w               = cam->GetFrameWidth();
        int h               = cam->GetFrameHeight();
        int fmt             = cam->GetFrameFormat();
        size_t sz           = cam->GetFrameSize();
        if (!data || w <= 0 || h <= 0) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        md.update(data, w, h);
        int mp    = md.motionPermille();
        bool trig = md.hasPrev() && (mp >= MOTION_TRIG_PERMILLE);
        mclog::tagInfo(TAG, "f{} motion={}permille TRIGGER={}", i, mp, trig ? 1 : 0);

        int64_t now = esp_timer_get_time();
        if (trig && (now - last_dump) > 2000000) {  // on motion, dump the frame (max every 2s)
            last_dump = now;
            mclog::tagInfo(TAG, "=== MOTION! dumping frame (motion={}permille) ===", mp);
            dump_jpeg_b64(data, sz, w, h, fmt);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    mclog::tagInfo(TAG, "===== MOTION VERIFY DONE =====");
    vTaskDelete(nullptr);
}

void sentry_selftest_run()
{
    xTaskCreatePinnedToCore(verify_task, "sentry_verify", 8192, nullptr, 4, nullptr, 1);
}

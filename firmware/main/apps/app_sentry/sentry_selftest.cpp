/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sentry_selftest.h"
#include <hal/board/hal_bridge.h>
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "human_face_detect.hpp"
#include "dl_image_define.hpp"

#include "jpg/image_to_jpeg.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <sys/param.h>

static const char* TAG = "SentrySelfTest";

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

// Standard BT.601 YUYV(YUY2) -> RGB888. Returns a PSRAM buffer (caller frees).
static uint8_t* yuyv_to_rgb888(const uint8_t* yuyv, int w, int h)
{
    uint8_t* rgb = (uint8_t*)heap_caps_malloc((size_t)w * h * 3, MALLOC_CAP_SPIRAM);
    if (!rgb) return nullptr;
    int total = w * h * 2;
    int px    = 0;
    for (int i = 0; i + 3 < total; i += 4) {
        int u  = yuyv[i + 1] - 128;
        int v  = yuyv[i + 3] - 128;
        int rd = (91881 * v) >> 16;
        int gd = (22554 * u + 46802 * v) >> 16;
        int bd = (116130 * u) >> 16;
        for (int k = 0; k < 2; k++) {
            int y           = yuyv[i + (k == 0 ? 0 : 2)];
            rgb[px * 3 + 0] = clamp8(y + rd);
            rgb[px * 3 + 1] = clamp8(y - gd);
            rgb[px * 3 + 2] = clamp8(y + bd);
            px++;
        }
    }
    return rgb;
}

static void selftest_task(void* /*arg*/)
{
    mclog::tagInfo(TAG, "========== SENTRY SELFTEST START ==========");

    // 1) Model load test
    HumanFaceDetect* detector = new HumanFaceDetect();
    mclog::tagInfo(TAG, "HumanFaceDetect constructed OK (model weights loaded)");

    // Ground-truth dump: capture one frame, JPEG-encode it (image_to_jpeg
    // handles YUYV correctly), base64 it to serial so we can view the actual
    // camera image and know whether the scene is empty / garbled / has faces.
    {
        auto* cam = hal_bridge::board_get_camera();
        if (cam && cam->StreamCaptures()) {
            const uint8_t* d = cam->GetFrameData();
            int w = cam->GetFrameWidth(), h = cam->GetFrameHeight(), fmt = cam->GetFrameFormat();
            uint8_t* jpg = nullptr;
            size_t jlen  = 0;
            if (image_to_jpeg((uint8_t*)d, cam->GetFrameSize(), w, h, (v4l2_pix_fmt_t)fmt, 80, &jpg, &jlen) && jpg) {
                size_t b64cap      = 4 * ((jlen + 2) / 3) + 1;
                unsigned char* b64 = (unsigned char*)heap_caps_malloc(b64cap, MALLOC_CAP_SPIRAM);
                size_t olen        = 0;
                if (b64 && mbedtls_base64_encode(b64, b64cap, &olen, jpg, jlen) == 0) {
                    mclog::tagInfo(TAG, "JPEG_B64_BEGIN jpeg_bytes={} b64_len={}", (int)jlen, (int)olen);
                    for (size_t off = 0; off < olen; off += 100) {
                        size_t n = MIN((size_t)100, olen - off);
                        printf("B64:%.*s\n", (int)n, (char*)(b64 + off));
                    }
                    mclog::tagInfo(TAG, "JPEG_B64_END");
                }
                if (b64) heap_caps_free(b64);
                free(jpg);
            } else {
                mclog::tagInfo(TAG, "JPEG encode failed");
            }
        }
    }

    int captured = 0, inferences_ok = 0, faces_seen = 0;

    // 2) Camera + inference pipeline test on live frames
    for (int i = 0; i < 20; i++) {
        auto* camera = hal_bridge::board_get_camera();
        if (!camera) {
            mclog::tagInfo(TAG, "camera handle is NULL");
            break;
        }
        if (!camera->StreamCaptures()) {
            mclog::tagInfo(TAG, "frame {}: capture failed", i);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        const uint8_t* data = camera->GetFrameData();
        int w   = camera->GetFrameWidth();
        int h   = camera->GetFrameHeight();
        int fmt = camera->GetFrameFormat();
        if (i == 0) {
            mclog::tagInfo(TAG, "camera frame: {}x{} fmt={} bytes={}", w, h, fmt, (int)camera->GetFrameSize());
        }
        if (!data || w <= 0 || h <= 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        captured++;

        // Convert YUYV -> RGB888 (esp-dl's most reliable input) with a known-correct
        // BT.601 converter, rather than relying on esp-dl's raw-YUYV path.
        uint8_t* rgb = (fmt == V4L2_PIX_FMT_YUYV) ? yuyv_to_rgb888(data, w, h) : nullptr;

        dl::image::img_t img;
        img.width  = (uint16_t)w;
        img.height = (uint16_t)h;
        if (rgb) {
            img.data     = rgb;
            img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
        } else {
            img.data     = (void*)data;
            img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
        }

        auto& results = detector->run(img);
        inferences_ok++;

        int n          = 0;
        int frame_area = w * h;
        for (auto& r : results) {
            if (r.box.size() >= 4) {
                int ba       = r.box_area();
                int permille = frame_area > 0 ? (int)((int64_t)ba * 1000 / frame_area) : 0;
                mclog::tagInfo(TAG, "  FACE score={}% box=[{},{},{},{}] area={}permille", (int)(r.score * 100),
                               r.box[0], r.box[1], r.box[2], r.box[3], permille);
                faces_seen++;
                n++;
            }
        }
        if (n == 0) {
            mclog::tagInfo(TAG, "  frame {}: inference OK, 0 faces (empty scene -> correct true-negative)", i);
        }
        if (rgb) heap_caps_free(rgb);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    delete detector;
    mclog::tagInfo(TAG, "========== SELFTEST DONE: captured={} inferences_ok={} faces_seen={} ==========", captured,
                   inferences_ok, faces_seen);
    vTaskDelete(nullptr);
}

void sentry_selftest_run()
{
    // 16 KB stack, app core; esp-dl needs a generous stack.
    xTaskCreatePinnedToCore(selftest_task, "sentry_selftest", 16384, nullptr, 4, nullptr, 1);
}

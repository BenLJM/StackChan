/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_sentry.h"
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <memory>

// esp-dl on-device face detection (component: espressif/human_face_detect)
#include "human_face_detect.hpp"
#include "dl_image_define.hpp"

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const char* TAG = "Sentry";

static std::unique_ptr<Button> _btn_quit;
static TaskHandle_t _detect_task = nullptr;
static std::atomic<bool> _running{false};
static std::atomic<bool> _task_done{false};

// Background detection loop. esp-dl needs a generous stack, so this runs on its
// own task (not the UI/main loop). It only logs for now (Phase 1).
static void detect_task(void* /*arg*/)
{
    mclog::tagInfo(TAG, "detect task start");

    // The detector loads model weights + allocates an arena; build it ONCE.
    HumanFaceDetect* detector = new HumanFaceDetect();

    while (_running.load()) {
        auto* camera = hal_bridge::board_get_camera();
        if (camera && camera->StreamCaptures()) {
            const uint8_t* data = camera->GetFrameData();
            int w   = camera->GetFrameWidth();
            int h   = camera->GetFrameHeight();
            int fmt = camera->GetFrameFormat();

            if (data && w > 0 && h > 0) {
                dl::image::img_t img;
                img.data   = (void*)data;
                img.width  = (uint16_t)w;
                img.height = (uint16_t)h;
                // The camera HAL outputs either RGB565 (display path) or YUYV.
                if (fmt == V4L2_PIX_FMT_YUYV) {
                    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_YUYV;
                } else {
                    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
                }

                auto& results       = detector->run(img);
                const int frame_area = w * h;
                int idx              = 0;
                for (auto& r : results) {
                    int box_area     = (r.box.size() >= 4) ? r.box_area() : 0;
                    int area_permille = (frame_area > 0) ? (int)((int64_t)box_area * 1000 / frame_area) : 0;
                    int score_pct    = (int)(r.score * 100.0f);
                    if (r.box.size() >= 4) {
                        mclog::tagInfo(TAG, "face #{} score={}% box=[{},{},{},{}] area={}permille",
                                       idx, score_pct, r.box[0], r.box[1], r.box[2], r.box[3], area_permille);
                    }
                    idx++;
                }
                if (idx == 0) {
                    mclog::tagInfo(TAG, "frame {}x{} fmt={} no face", w, h, fmt);
                }
            }
        } else {
            mclog::tagInfo(TAG, "camera capture failed");
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    delete detector;
    mclog::tagInfo(TAG, "detect task exit");
    _task_done.store(true);
    vTaskDelete(nullptr);
}

AppSentry::AppSentry()
{
    setAppInfo().name = "SENTRY";
}

void AppSentry::onCreate()
{
    mclog::tagInfo(TAG, "on create");
}

void AppSentry::onOpen()
{
    mclog::tagInfo(TAG, "on open");

    {
        LvglLockGuard lock;
        _btn_quit = std::make_unique<Button>(lv_screen_active());
        _btn_quit->setAlign(LV_ALIGN_CENTER);
        _btn_quit->label().setText(LV_SYMBOL_CLOSE " EXIT");
        _btn_quit->onClick().connect([this]() { close(); });
    }

    // Start the detection task (16 KB stack, pinned to the app core).
    _running.store(true);
    _task_done.store(false);
    xTaskCreatePinnedToCore(detect_task, "sentry_detect", 16384, nullptr, 4, &_detect_task, 1);
}

void AppSentry::onRunning() {}

void AppSentry::onClose()
{
    mclog::tagInfo(TAG, "on close");

    // Ask the detection task to stop and wait (up to ~2s) for it to finish.
    _running.store(false);
    for (int i = 0; i < 200 && !_task_done.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    _detect_task = nullptr;

    LvglLockGuard lock;
    _btn_quit.reset();
}

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_sentry.h"
#include "motion_detect.h"
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include "jpg/image_to_jpeg.h"
#include <lvgl.h>
#include <cstdio>
#include <memory>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const char* TAG = "Sentry";

// Tunables
static const int MOTION_TRIG_PERMILLE = 60;     // 6% of frame changed = motion
static const int CONSECUTIVE_HITS     = 2;      // require N consecutive motion samples
static const uint32_t GRACE_MS        = 8000;   // arming countdown (leave the frame)
static const uint32_t ALERT_MS        = 4000;   // how long the warning stays up
static const uint32_t COOLDOWN_MS     = 30000;  // min gap between alerts
static const uint32_t CHECK_MS        = 150;    // motion sampling period
static const char* WARN_TEXT          = "\xE9\x9D\x9E\xE8\xAF\xB7\xE5\x8B\xBF\xE5\x8A\xA8";  // 非请勿动 (UTF-8)

enum SentryState { ST_ARMING, ST_ARMED, ST_ALERTING, ST_COOLDOWN };

static std::unique_ptr<Button> _btn_quit;
static lv_obj_t* _label_status = nullptr;
static lv_obj_t* _label_warn   = nullptr;
static MotionDetector _md;
static SentryState _state;
static uint32_t _state_ms;
static uint32_t _last_check_ms;
static int _hits;

// --- helpers (LVGL helpers must be called under LvglLockGuard) ---
static void set_status(const char* txt)
{
    if (_label_status) lv_label_set_text(_label_status, txt);
}

static void show_warning_ui(bool on)
{
    if (_label_warn) {
        if (on) {
            lv_obj_clear_flag(_label_warn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_label_warn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(on ? 0x500000 : 0x000000), 0);
}

// Capture the current camera frame as JPEG (the "intruder photo"). Phase B2 logs
// it; Phase B3 will POST it to Telegram instead of freeing it.
static void capture_intruder_photo()
{
    auto* cam = hal_bridge::board_get_camera();
    if (!cam) return;
    const uint8_t* data = cam->GetFrameData();
    int w               = cam->GetFrameWidth();
    int h               = cam->GetFrameHeight();
    int fmt             = cam->GetFrameFormat();
    size_t sz           = cam->GetFrameSize();
    if (!data || w <= 0 || h <= 0) return;

    uint8_t* jpg = nullptr;
    size_t jlen  = 0;
    if (image_to_jpeg((uint8_t*)data, sz, w, h, (v4l2_pix_fmt_t)fmt, 80, &jpg, &jlen) && jpg) {
        mclog::tagInfo(TAG, "captured intruder photo: {} bytes (Telegram upload = Phase B3)", (int)jlen);
        free(jpg);
    }
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
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);

        _label_status = lv_label_create(lv_screen_active());
        lv_obj_align(_label_status, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_style_text_color(_label_status, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(_label_status, "SENTRY");

        _label_warn = lv_label_create(lv_screen_active());
        lv_obj_align(_label_warn, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(_label_warn, lv_color_hex(0xFF3030), 0);
        // Use the theme default font (font_puhui) which has Chinese glyphs.
        lv_label_set_text(_label_warn, WARN_TEXT);
        lv_obj_add_flag(_label_warn, LV_OBJ_FLAG_HIDDEN);

        _btn_quit = std::make_unique<Button>(lv_screen_active());
        _btn_quit->setAlign(LV_ALIGN_BOTTOM_MID);
        _btn_quit->label().setText(LV_SYMBOL_CLOSE " EXIT");
        _btn_quit->onClick().connect([this]() { close(); });
    }

    GetHAL().showRgbColor(0, 0, 0);
    _md.reset();
    _hits          = 0;
    _last_check_ms = 0;
    _state         = ST_ARMING;
    _state_ms      = GetHAL().millis();
}

void AppSentry::onRunning()
{
    uint32_t now = GetHAL().millis();
    if (now - _last_check_ms < CHECK_MS) {
        return;
    }
    _last_check_ms = now;

    // Sample motion (camera/JPEG are NOT LVGL — do them without the LVGL lock).
    int mp    = 0;
    bool have = false;
    auto* cam = hal_bridge::board_get_camera();
    if (cam && cam->StreamCaptures()) {
        const uint8_t* data = cam->GetFrameData();
        int w               = cam->GetFrameWidth();
        int h               = cam->GetFrameHeight();
        if (data && w > 0 && h > 0 && _md.update(data, w, h)) {
            mp   = _md.motionPermille();
            have = _md.hasPrev();
        }
    }

    bool do_capture = false;

    {
        LvglLockGuard lock;
        switch (_state) {
        case ST_ARMING: {
            if (now - _state_ms > GRACE_MS) {
                _state = ST_ARMED;
                _hits  = 0;
                set_status("ARMED");
            } else {
                char buf[24];
                snprintf(buf, sizeof(buf), "ARMING %lus", (unsigned long)((GRACE_MS - (now - _state_ms)) / 1000 + 1));
                set_status(buf);
            }
            break;
        }
        case ST_ARMED: {
            if (have && mp >= MOTION_TRIG_PERMILLE) {
                _hits++;
            } else {
                _hits = 0;
            }
            if (_hits >= CONSECUTIVE_HITS) {
                _hits     = 0;
                _state    = ST_ALERTING;
                _state_ms = now;
                set_status("ALERT");
                show_warning_ui(true);
                do_capture = true;
                mclog::tagInfo(TAG, "ALERT: motion={}permille", mp);
            }
            break;
        }
        case ST_ALERTING: {
            if (now - _state_ms > ALERT_MS) {
                _state    = ST_COOLDOWN;
                _state_ms = now;
                show_warning_ui(false);
                set_status("COOLDOWN");
            }
            break;
        }
        case ST_COOLDOWN: {
            if (now - _state_ms > COOLDOWN_MS) {
                _state = ST_ARMED;
                _md.reset();
                set_status("ARMED");
            }
            break;
        }
        }
    }

    // LED + photo outside the LVGL lock.
    if (_state == ST_ALERTING && do_capture) {
        GetHAL().showRgbColor(255, 0, 0);
        capture_intruder_photo();
    } else if (_state == ST_COOLDOWN && now - _state_ms < CHECK_MS * 2) {
        GetHAL().showRgbColor(0, 0, 0);  // turn LEDs off as we enter cooldown
    }
}

void AppSentry::onClose()
{
    mclog::tagInfo(TAG, "on close");
    GetHAL().showRgbColor(0, 0, 0);

    LvglLockGuard lock;
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);
    if (_label_status) { lv_obj_del(_label_status); _label_status = nullptr; }
    if (_label_warn) { lv_obj_del(_label_warn); _label_warn = nullptr; }
    _btn_quit.reset();
}

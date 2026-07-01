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
#include <stackchan/stackchan.h>
#include <stackchan/motion/motion.h>
#include <stackchan/avatar/avatar.h>
#include <stackchan/modifiers/blink.h>
#include <stackchan/modifiers/breath.h>
#include "jpg/image_to_jpeg.h"
#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <math.h>

#include <board.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ping/ping_sock.h"
extern "C" {
#include "lwip/ip_addr.h"
}

// Telegram credentials live in a git-ignored header. If it's absent, Telegram push is
// compiled out (the sentry still works, just logs the capture).
#if __has_include("tg_secret.h")
#include "tg_secret.h"
#define HAVE_TG 1
#endif

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const char* TAG = "Sentry";

// Custom 48px font (app_sentry/sentry_font_48.c) with exactly the glyphs this screen shows.
LV_FONT_DECLARE(sentry_font_48);

// ---- Owner presence (WiFi) ----
static const char* OWNER_IP = "192.168.66.184";

// State messages (UTF-8). All glyphs are present in sentry_font_48.
static const char* TXT_ARM    = "\xE5\xB8\x83\xE9\x98\xB2";                          // 布防
static const char* TXT_WARN   = "\xE9\x9D\x9E\xE8\xAF\xB7\xE5\x8B\xBF\xE5\x8A\xA8";  // 非请勿动
static const char* TXT_COOL   = "\xE5\x86\xB7\xE5\x8D\xB4\xE4\xB8\xAD";              // 冷却中
static const char* TXT_NOWIFI = "\xE7\xBD\x91\xE7\xBB\x9C\xE6\x96\xAD\xE5\xBC\x80";  // 网络断开

// ---- Detection tunables ----
static const int MOTION_TRIG_PERMILLE   = 40;
static const int MOTION_STRONG_PERMILLE = 150;
static const int CONSECUTIVE_HITS       = 2;
static const int PIX_THRESHOLD          = 18;
static const uint32_t GRACE_MS          = 5000;
static const uint32_t ALERT_MS          = 4000;
static const uint32_t COOLDOWN_MS       = 15000;
static const uint32_t CHECK_MS          = 100;

// ---- Presence tunables ----
static const uint32_t PING_INTERVAL_MS = 4000;
static const uint32_t PING_TIMEOUT_MS  = 1500;
static const uint32_t AWAY_CONFIRM_MS  = 30000;  // phone unseen this long -> "away" (~30 s)
static const uint32_t LOG_MS           = 5000;

// ---- Head-shake tunables ----
static const int SHAKE_AMP          = 300;
static const int SHAKE_SPEED        = 900;
static const uint32_t SHAKE_HALF_MS = 150;
static const uint32_t LED_FLASH_MS  = 250;

enum SentryState { ST_NOWIFI, ST_DISARMED, ST_ARMING, ST_ARMED, ST_ALERTING, ST_COOLDOWN };
enum PresenceCond { C_NOWIFI, C_PRESENT, C_AWAY };
enum View { V_LABEL, V_AVATAR, V_DOT };

static std::unique_ptr<Button> _btn_quit;
static lv_obj_t* _scr           = nullptr;
static lv_obj_t* _prev_scr      = nullptr;
static lv_obj_t* _label_main    = nullptr;
static lv_obj_t* _avatar_holder = nullptr;  // holds the StackChan face (shown when disarmed)
static lv_obj_t* _dot           = nullptr;  // animated red "sentry eye" (shown when armed)
static int _blink_id            = -1;
static int _breath_id           = -1;
static MotionDetector _md;
static SentryState _state;
static PresenceCond _cond;
static uint32_t _state_ms;
static uint32_t _last_check_ms;
static uint32_t _last_probe_ms;
static volatile uint32_t _present_seen_ms;  // stamped by the ping-reply callback (other task)
static bool _wifi_ok;
static int _hits;
static int _shake_dir;
static uint32_t _shake_ms;

// ---- Owner presence via a background ICMP ping session ----
static esp_ping_handle_t _ping_handle = nullptr;
static bool _ping_started             = false;
static bool _wifi_started             = false;

static void on_ping_success(esp_ping_handle_t hdl, void* args)
{
    _present_seen_ms = GetHAL().millis();
}

static void start_ping_session()
{
    ip_addr_t target = {};
    if (!ipaddr_aton(OWNER_IP, &target)) {
        return;
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr       = target;
    cfg.count             = 0;  // infinite
    cfg.interval_ms       = PING_INTERVAL_MS;
    cfg.timeout_ms        = PING_TIMEOUT_MS;

    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success      = on_ping_success;

    if (esp_ping_new_session(&cfg, &cbs, &_ping_handle) == ESP_OK && _ping_handle) {
        esp_ping_start(_ping_handle);
        mclog::tagInfo(TAG, "ping session started -> {}", OWNER_IP);
    }
}

// ---- Telegram push (runs in a background task; TLS needs a big stack) ----
static bool send_telegram_photo(const char* token, const char* chat_id, const uint8_t* jpg, size_t len)
{
    if (!token || !chat_id || !jpg || len == 0) return false;

    std::string url  = std::string("https://api.telegram.org/bot") + token + "/sendPhoto";
    auto network     = Board::GetInstance().GetNetwork();
    auto http        = network->CreateHttp(0);
    if (!http) return false;

    std::string boundary = "----StackChanSentryBoundary";
    http->SetTimeout(15000);
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", url)) {
        http->Close();
        return false;
    }

    std::string p1 = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
                     chat_id + "\r\n";
    http->Write(p1.c_str(), p1.size());

    std::string p2 = "--" + boundary +
                     "\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"intruder.jpg\"\r\n"
                     "Content-Type: image/jpeg\r\n\r\n";
    http->Write(p2.c_str(), p2.size());

    http->Write((const char*)jpg, len);

    std::string p3 = "\r\n--" + boundary + "--\r\n";
    http->Write(p3.c_str(), p3.size());
    http->Write("", 0);  // chunked terminator

    int status = http->GetStatusCode();
    http->Close();
    mclog::tagInfo(TAG, "telegram HTTP status {}", status);
    return status == 200;
}

struct TgJob {
    uint8_t* jpg;
    size_t len;
};
static volatile bool _tg_busy = false;

static void tg_send_task(void* arg)
{
    TgJob* job = static_cast<TgJob*>(arg);
#ifdef HAVE_TG
    bool ok = send_telegram_photo(TG_BOT_TOKEN, TG_CHAT_ID, job->jpg, job->len);
    mclog::tagInfo(TAG, "telegram send: {}", ok ? "ok" : "FAILED");
#endif
    free(job->jpg);
    delete job;
    _tg_busy = false;
    vTaskDelete(nullptr);
}

// ---- LVGL helpers (call under LvglLockGuard) ----
static void set_main(const char* txt, uint32_t color)
{
    if (_label_main) {
        lv_label_set_text(_label_main, txt);
        lv_obj_set_style_text_color(_label_main, lv_color_hex(color), 0);
    }
}

static void set_alert_bg(bool on)
{
    if (_scr) {
        lv_obj_set_style_bg_color(_scr, lv_color_hex(on ? 0xC00000 : 0x000000), 0);
    }
}

static void show_view(View v)
{
    auto vis = [](lv_obj_t* o, bool show) {
        if (!o) return;
        if (show)
            lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    vis(_label_main, v == V_LABEL);
    vis(_avatar_holder, v == V_AVATAR);
    vis(_dot, v == V_DOT);
}

// Breathing red "sentry eye" — evil-robot glow. Call under LvglLockGuard when ST_ARMED.
static void animate_dot(uint32_t now)
{
    if (!_dot) return;
    float ph = (float)(now % 1800) / 1800.0f;
    float s  = 0.5f - 0.5f * cosf(ph * 6.2831853f);  // 0..1..0 smooth
    int diam = 46 + (int)(s * 40.0f);                // 46..86 px
    int opa  = 110 + (int)(s * 145.0f);              // 110..255
    int glow = 8 + (int)(s * 48.0f);                 // 8..56
    lv_obj_set_size(_dot, diam, diam);
    lv_obj_set_style_bg_opa(_dot, opa, 0);
    lv_obj_set_style_shadow_width(_dot, glow, 0);
    lv_obj_center(_dot);
}

// Capture the current camera frame as JPEG and push it to Telegram (background task).
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
    if (!image_to_jpeg((uint8_t*)data, sz, w, h, (v4l2_pix_fmt_t)fmt, 80, &jpg, &jlen) || !jpg) {
        return;
    }
    mclog::tagInfo(TAG, "captured intruder photo: {} bytes", (int)jlen);

#ifdef HAVE_TG
    if (!_tg_busy) {
        _tg_busy   = true;
        TgJob* job = new TgJob{jpg, jlen};
        // TLS handshake is stack-heavy -> generous stack; task frees the JPEG.
        if (xTaskCreate(tg_send_task, "tg_send", 24576, job, 4, nullptr) == pdPASS) {
            return;  // task owns jpg now
        }
        delete job;
        _tg_busy = false;
    }
#endif
    free(jpg);
}

static void alert_motion_begin()
{
    auto& m = GetStackChan().motion();
    m.setAutoTorqueReleaseEnabled(false);
    m.setTorqueEnabled(true);
    _shake_dir = 1;
    _shake_ms  = 0;
}

static void alert_motion_end()
{
    auto& m = GetStackChan().motion();
    m.moveYawWithSpeed(0, 500);
    m.setAutoTorqueReleaseEnabled(true);
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

        _prev_scr = lv_screen_active();
        _scr      = lv_obj_create(NULL);
        lv_obj_remove_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(0x000000), 0);
        lv_obj_set_style_pad_all(_scr, 0, 0);

        // (a) Text status message (network/arming/warning/cooldown).
        _label_main = lv_label_create(_scr);
        lv_obj_set_style_text_font(_label_main, &sentry_font_48, 0);
        lv_obj_set_style_text_align(_label_main, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(_label_main, LV_ALIGN_CENTER, 0, -6);
        lv_obj_set_style_text_color(_label_main, lv_color_hex(0xFF8800), 0);
        lv_label_set_text(_label_main, TXT_NOWIFI);

        // (b) The normal StackChan face (shown when disarmed / owner present).
        _avatar_holder = lv_obj_create(_scr);
        lv_obj_remove_flag(_avatar_holder, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(_avatar_holder, 320, 240);
        lv_obj_center(_avatar_holder);
        lv_obj_set_style_bg_opa(_avatar_holder, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_avatar_holder, 0, 0);
        lv_obj_set_style_pad_all(_avatar_holder, 0, 0);
        lv_obj_add_flag(_avatar_holder, LV_OBJ_FLAG_HIDDEN);

        // (c) Animated red "sentry eye" (shown when armed / monitoring).
        _dot = lv_obj_create(_scr);
        lv_obj_remove_flag(_dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(_dot, 60, 60);
        lv_obj_center(_dot);
        lv_obj_set_style_radius(_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(_dot, lv_color_hex(0xFF1010), 0);
        lv_obj_set_style_border_width(_dot, 0, 0);
        lv_obj_set_style_shadow_color(_dot, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_shadow_width(_dot, 30, 0);
        lv_obj_add_flag(_dot, LV_OBJ_FLAG_HIDDEN);

        _btn_quit = std::make_unique<Button>(_scr);
        _btn_quit->setAlign(LV_ALIGN_BOTTOM_MID);
        _btn_quit->label().setText("EXIT");
        _btn_quit->onClick().connect([this]() { close(); });

        // The StackChan face + its blink/breathe animation.
        if (!GetStackChan().hasAvatar()) {
            auto avatar = std::make_unique<stackchan::avatar::DefaultAvatar>();
            avatar->init(_avatar_holder);
            GetStackChan().attachAvatar(std::move(avatar));
            _breath_id = GetStackChan().addModifier(std::make_unique<stackchan::BreathModifier>());
            _blink_id  = GetStackChan().addModifier(std::make_unique<stackchan::BlinkModifier>());
        }

        show_view(V_LABEL);
        lv_screen_load(_scr);
    }

    // Keep the head still & under our control (no idle drift moving the camera).
    GetStackChan().motion().setModifyLock(true);

    // Bring WiFi up (needed for presence detection AND the Telegram push).
    if (!_wifi_started) {
        _wifi_started = true;
        Board::GetInstance().StartNetwork();  // non-blocking; connects via WiFi events
    }

    GetHAL().showRgbColor(0, 0, 0);
    _md.pixThreshold = PIX_THRESHOLD;
    _md.reset();
    _hits            = 0;
    _last_check_ms   = 0;
    _last_probe_ms   = 0;
    _wifi_ok         = false;
    _present_seen_ms = GetHAL().millis();
    _cond            = C_NOWIFI;
    _state           = ST_NOWIFI;
    _state_ms        = GetHAL().millis();
}

void AppSentry::onRunning()
{
    uint32_t now = GetHAL().millis();

    // (1) Keep our screen on top; keep the launcher's 30s screensaver disarmed.
    {
        LvglLockGuard lock;
        lv_display_trigger_activity(NULL);
        if (_scr && lv_screen_active() != _scr) {
            lv_screen_load(_scr);
        }
    }

    // (2) Pump servo + avatar animation.
    GetStackChan().update();

    // (3) Per-state animations.
    if (_state == ST_ALERTING) {
        bool led_on = ((now - _state_ms) / LED_FLASH_MS) % 2 == 0;
        GetHAL().showRgbColor(led_on ? 255 : 0, 0, 0);
        if (now - _shake_ms >= SHAKE_HALF_MS) {
            _shake_ms  = now;
            _shake_dir = -_shake_dir;
            GetStackChan().motion().moveYawWithSpeed(_shake_dir * SHAKE_AMP, SHAKE_SPEED);
        }
    } else if (_state == ST_ARMED) {
        static uint32_t _last_dot_ms = 0;
        if (now - _last_dot_ms >= 40) {  // ~25 fps; don't invalidate LVGL every loop
            _last_dot_ms = now;
            LvglLockGuard lock;
            animate_dot(now);
        }
    }

    // (4) Presence: WiFi status + background-ping freshness.
    _wifi_ok = (GetHAL().getWifiStatus() != WifiStatus::None);
    if (_wifi_ok && !_ping_started) {
        _ping_started = true;
        GetHAL().startSntp();  // correct system time for TLS (RTC already gives a baseline)
        start_ping_session();
    }
    if (now - _last_probe_ms >= LOG_MS) {
        _last_probe_ms = now;
        mclog::tagInfo(TAG, "presence: wifi={} last_reply={}s_ago", _wifi_ok,
                       (int)((now - _present_seen_ms) / 1000));
    }

    // (5) Outer presence gate.
    PresenceCond cond;
    if (!_wifi_ok) {
        cond = C_NOWIFI;
    } else if (now - _present_seen_ms < AWAY_CONFIRM_MS) {
        cond = C_PRESENT;
    } else {
        cond = C_AWAY;
    }

    if (cond != _cond) {
        bool was_alerting = (_state == ST_ALERTING);
        _cond             = cond;
        {
            LvglLockGuard lock;
            set_alert_bg(false);
            if (cond == C_NOWIFI) {
                _state = ST_NOWIFI;
                show_view(V_LABEL);
                set_main(TXT_NOWIFI, 0xFF8800);
            } else if (cond == C_PRESENT) {
                _state = ST_DISARMED;
                show_view(V_AVATAR);  // normal StackChan face, no text
            } else {                  // C_AWAY -> begin arming
                _state    = ST_ARMING;
                _state_ms = now;
                _hits     = 0;
                show_view(V_LABEL);
                set_main(TXT_ARM, 0xFFAA00);
            }
        }
        if (cond == C_AWAY) {
            _md.reset();
        } else {
            GetHAL().showRgbColor(0, 0, 0);
            if (was_alerting) alert_motion_end();
        }
    }

    // Only run intrusion detection while AWAY.
    if (cond != C_AWAY) {
        return;
    }

    // (6) Throttle camera/motion sampling.
    if (now - _last_check_ms < CHECK_MS) {
        return;
    }
    _last_check_ms = now;

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

    bool enter_alert = false;
    bool exit_alert  = false;

    {
        LvglLockGuard lock;

        switch (_state) {
        case ST_ARMING: {
            if (now - _state_ms > GRACE_MS) {
                _state = ST_ARMED;
                _hits  = 0;
                show_view(V_DOT);
            } else {
                char buf[20];
                snprintf(buf, sizeof(buf), "%s %lu", TXT_ARM,
                         (unsigned long)((GRACE_MS - (now - _state_ms)) / 1000 + 1));
                set_main(buf, 0xFFAA00);
            }
            break;
        }
        case ST_ARMED: {
            if (have && mp >= MOTION_STRONG_PERMILLE) {
                _hits = CONSECUTIVE_HITS;
            } else if (have && mp >= MOTION_TRIG_PERMILLE) {
                _hits++;
            } else {
                _hits = 0;
            }
            if (_hits >= CONSECUTIVE_HITS) {
                _hits       = 0;
                _state      = ST_ALERTING;
                _state_ms   = now;
                enter_alert = true;
                show_view(V_LABEL);
                set_main(TXT_WARN, 0xFFFFFF);
                set_alert_bg(true);
                mclog::tagInfo(TAG, "ALERT: motion={}permille", mp);
            }
            break;
        }
        case ST_ALERTING: {
            if (now - _state_ms > ALERT_MS) {
                _state     = ST_COOLDOWN;
                _state_ms  = now;
                exit_alert = true;
                set_alert_bg(false);
                show_view(V_LABEL);
                set_main(TXT_COOL, 0xAAAAAA);
            }
            break;
        }
        case ST_COOLDOWN: {
            if (now - _state_ms > COOLDOWN_MS) {
                _state = ST_ARMED;
                _md.reset();
                show_view(V_DOT);
            }
            break;
        }
        default:
            break;
        }
    }

    if (enter_alert) {
        GetHAL().showRgbColor(255, 0, 0);
        alert_motion_begin();
        capture_intruder_photo();
    }
    if (exit_alert) {
        GetHAL().showRgbColor(0, 0, 0);
        alert_motion_end();
    }
}

void AppSentry::onClose()
{
    mclog::tagInfo(TAG, "on close");
    GetHAL().showRgbColor(0, 0, 0);

    auto& m = GetStackChan().motion();
    m.setAutoTorqueReleaseEnabled(true);
    m.setModifyLock(false);
    m.goHome(400);

    if (_blink_id >= 0) {
        GetStackChan().removeModifier(_blink_id);
        _blink_id = -1;
    }
    if (_breath_id >= 0) {
        GetStackChan().removeModifier(_breath_id);
        _breath_id = -1;
    }
    GetStackChan().resetAvatar();

    LvglLockGuard lock;
    if (_prev_scr) {
        lv_screen_load(_prev_scr);
        _prev_scr = nullptr;
    }
    _btn_quit.reset();
    if (_scr) {
        lv_obj_del(_scr);
        _scr = nullptr;
    }
    _label_main    = nullptr;
    _avatar_holder = nullptr;
    _dot           = nullptr;
}

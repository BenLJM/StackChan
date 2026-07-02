/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_sentry.h"
#include "motion_detect.h"
#include "person_detect.h"
#include <esp_heap_caps.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <stackchan/motion/motion.h>
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

// ---- Person verification tunables ----
// Motion alone no longer fires the alert: it opens a VERIFY window during which the
// TFLM person classifier must confirm a human. The photo sent is the highest-scoring
// frame ("best shot"), not whatever was in the buffer at the motion instant.
static const int PERSON_THRESH_PCT = 55;    // person score (%) to confirm an intruder
static const int PERSON_GREAT_PCT  = 85;    // good enough to stop hunting a better shot
static const uint32_t VERIFY_MS    = 5000;  // max hunt for a person after motion
static const uint32_t IMPROVE_MS   = 1500;  // after confirm, keep hunting a better shot
static const uint32_t INFER_GAP_MS = 350;   // min gap between inferences (each ~0.5 s)

// ---- Alert-fatigue suppression ("visit" model) ----
// We can't biometrically recognize "the same person" on this chip, so sameness is
// temporal: person sightings closer together than REALERT_GAP_MS count as ONE ongoing
// visit. Only the FIRST sighting of a visit fires the full alert + Telegram; later
// sightings stay silent (serial log only) and slide the window. The sentry eye turns
// ORANGE while a visit is being suppressed, back to red once the visitor is long gone.
static const uint32_t REALERT_GAP_MS = 10 * 60 * 1000;  // re-alert only after 10 min unseen

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

enum SentryState { ST_NOWIFI, ST_DISARMED, ST_ARMING, ST_ARMED, ST_VERIFY, ST_ALERTING, ST_COOLDOWN };
enum PresenceCond { C_NOWIFI, C_PRESENT, C_AWAY };
enum View { V_LABEL, V_AVATAR, V_DOT };

static std::unique_ptr<Button> _btn_quit;
static lv_obj_t* _scr           = nullptr;
static lv_obj_t* _prev_scr      = nullptr;
static lv_obj_t* _label_main    = nullptr;
static lv_obj_t* _face   = nullptr;  // StackChan face container (shown when disarmed)
static lv_obj_t* _eye_l  = nullptr;
static lv_obj_t* _eye_r  = nullptr;
static lv_obj_t* _mouth  = nullptr;
static lv_obj_t* _dot    = nullptr;  // animated red "sentry eye" (shown when armed)
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

// ---- Person-verify state (best-shot buffer lives in PSRAM) ----
static const size_t FRAME_MAX  = 320 * 240 * 2;  // YUYV frame size
static uint8_t* _best_frame    = nullptr;
static size_t _best_len        = 0;
static int _best_w             = 0;
static int _best_h             = 0;
static int _best_fmt           = 0;
static int _best_score         = -1;
static uint32_t _confirm_ms    = 0;  // when a person was confirmed (0 = not yet)
static uint32_t _last_infer_ms = 0;
static bool _pd_ok             = false;  // person model usable? (else fall back to motion-only)
static uint32_t _last_person_ms = 0;     // last confirmed person sighting (0 = none since arming)

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
    vis(_face, v == V_AVATAR);
    vis(_dot, v == V_DOT);
}

// Cute StackChan-style blink. Call under LvglLockGuard when ST_DISARMED.
static void animate_face(uint32_t now)
{
    if (!_eye_l || !_eye_r) return;
    bool blink = (now % 3600) < 130;  // quick blink every 3.6 s
    int h      = blink ? 6 : 46;
    lv_obj_set_height(_eye_l, h);
    lv_obj_set_height(_eye_r, h);
    lv_obj_align(_eye_l, LV_ALIGN_CENTER, -50, -14);
    lv_obj_align(_eye_r, LV_ALIGN_CENTER, 50, -14);
}

// Breathing "sentry eye" — evil-robot glow. Red = fully vigilant; orange = a recent
// visitor is being suppressed (still watching, holding fire). Call under LvglLockGuard.
static void animate_dot(uint32_t now)
{
    if (!_dot) return;
    bool quiet = (_last_person_ms != 0) && (now - _last_person_ms < REALERT_GAP_MS);
    lv_obj_set_style_bg_color(_dot, lv_color_hex(quiet ? 0xFF8800 : 0xFF1010), 0);
    lv_obj_set_style_shadow_color(_dot, lv_color_hex(quiet ? 0xFF8800 : 0xFF0000), 0);
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

// Encode the best person-frame (fallback: live frame) as JPEG and push it to Telegram.
static void send_best_photo()
{
    const uint8_t* data = _best_frame;
    size_t sz           = _best_len;
    int w = _best_w, h = _best_h, fmt = _best_fmt;

    if (!data || sz == 0) {  // shouldn't happen, but never send nothing
        auto* cam = hal_bridge::board_get_camera();
        if (!cam) return;
        data = cam->GetFrameData();
        sz   = cam->GetFrameSize();
        w    = cam->GetFrameWidth();
        h    = cam->GetFrameHeight();
        fmt  = cam->GetFrameFormat();
        if (!data || w <= 0 || h <= 0) return;
    }

    uint8_t* jpg = nullptr;
    size_t jlen  = 0;
    if (!image_to_jpeg((uint8_t*)data, sz, w, h, (v4l2_pix_fmt_t)fmt, 80, &jpg, &jlen) || !jpg) {
        return;
    }
    mclog::tagInfo(TAG, "intruder photo: {} bytes (person {}%)", (int)jlen, _best_score);

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

        // (b) The StackChan face (shown when disarmed / owner present): two white eyes +
        //     a mouth on black, drawn directly so it always renders. Blinks via animate_face().
        _face = lv_obj_create(_scr);
        lv_obj_remove_flag(_face, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(_face, 320, 240);
        lv_obj_center(_face);
        lv_obj_set_style_bg_color(_face, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(_face, 0, 0);
        lv_obj_set_style_pad_all(_face, 0, 0);

        auto make_white = [&](int w, int h, int rad, int dx, int dy) {
            lv_obj_t* o = lv_obj_create(_face);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(o, w, h);
            lv_obj_set_style_radius(o, rad, 0);
            lv_obj_set_style_bg_color(o, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(o, 0, 0);
            lv_obj_align(o, LV_ALIGN_CENTER, dx, dy);
            return o;
        };
        _eye_l = make_white(40, 46, 12, -50, -14);
        _eye_r = make_white(40, 46, 12, 50, -14);
        _mouth = make_white(70, 12, 6, 0, 52);
        lv_obj_add_flag(_face, LV_OBJ_FLAG_HIDDEN);

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
    _confirm_ms      = 0;
    _best_score      = -1;
    _best_len        = 0;
    _last_person_ms  = 0;
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

    // (3) Per-state animations. The alert (LED + shake) also runs while a confirmed
    // person is still in the VERIFY improve-window.
    bool alert_active = (_state == ST_ALERTING) || (_state == ST_VERIFY && _confirm_ms != 0);
    if (alert_active) {
        bool led_on = ((now - _state_ms) / LED_FLASH_MS) % 2 == 0;
        GetHAL().showRgbColor(led_on ? 255 : 0, 0, 0);
        if (now - _shake_ms >= SHAKE_HALF_MS) {
            _shake_ms  = now;
            _shake_dir = -_shake_dir;
            GetStackChan().motion().moveYawWithSpeed(_shake_dir * SHAKE_AMP, SHAKE_SPEED);
        }
    } else if (_state == ST_ARMED || _state == ST_VERIFY || _state == ST_COOLDOWN) {
        static uint32_t _last_dot_ms = 0;
        if (now - _last_dot_ms >= 40) {  // ~25 fps; don't invalidate LVGL every loop
            _last_dot_ms = now;
            LvglLockGuard lock;
            animate_dot(now);
        }
    } else if (_state == ST_DISARMED) {
        static uint32_t _last_face_ms = 0;
        if (now - _last_face_ms >= 40) {
            _last_face_ms = now;
            LvglLockGuard lock;
            animate_face(now);
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
        int32_t ago = (int32_t)(now - _present_seen_ms);
        if (ago < 0) ago = 0;
        mclog::tagInfo(TAG, "presence: wifi={} last_reply={}s_ago", _wifi_ok, (int)(ago / 1000));
    }

    // (5) Outer presence gate.
    PresenceCond cond;
    // Signed delta: the ping callback (another task) may stamp _present_seen_ms AFTER
    // `now` was read this loop — unsigned math would underflow to ~4e9 and flash "away".
    int32_t unseen_ms = (int32_t)(now - _present_seen_ms);
    if (unseen_ms < 0) unseen_ms = 0;
    if (!_wifi_ok) {
        cond = C_NOWIFI;
    } else if ((uint32_t)unseen_ms < AWAY_CONFIRM_MS) {
        cond = C_PRESENT;
    } else {
        cond = C_AWAY;
    }

    if (cond != _cond) {
        bool was_alerting = (_state == ST_ALERTING) || (_state == ST_VERIFY && _confirm_ms != 0);
        _cond             = cond;
        _confirm_ms       = 0;
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
            _last_person_ms = 0;  // fresh arming: first sighting always alerts
            _pd_ok = person_detect_init();  // lazy: model + arena only when actually arming
            if (!_pd_ok) {
                mclog::tagWarn(TAG, "person model unavailable -> motion-only alerts");
            }
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

    int mp               = 0;
    bool have            = false;
    const uint8_t* fdata = nullptr;
    int fw = 0, fh = 0, ffmt = 0;
    size_t flen = 0;
    auto* cam   = hal_bridge::board_get_camera();
    if (cam && cam->StreamCaptures()) {
        fdata = cam->GetFrameData();
        fw    = cam->GetFrameWidth();
        fh    = cam->GetFrameHeight();
        ffmt  = cam->GetFrameFormat();
        flen  = cam->GetFrameSize();
        if (fdata && fw > 0 && fh > 0 && _md.update(fdata, fw, fh)) {
            mp   = _md.motionPermille();
            have = _md.hasPrev();
        }
    }

    // While verifying: ask the person classifier (blocking ~0.5 s — done OUTSIDE the
    // LVGL lock so rendering never stalls). Keep a copy of the best-scoring frame.
    int person = -1;
    if (_state == ST_VERIFY) {
        if (!_pd_ok) {
            person = 100;  // model unavailable -> behave like the old motion-only sentry
        } else if (fdata && now - _last_infer_ms >= INFER_GAP_MS) {
            _last_infer_ms = now;
            person         = person_detect_score(fdata, fw, fh);
            mclog::tagInfo(TAG, "person score: {}% (best {}%)", person, _best_score);
            if (person > _best_score && flen > 0 && flen <= FRAME_MAX) {
                if (!_best_frame) {
                    _best_frame = (uint8_t*)heap_caps_malloc(FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (_best_frame) {
                    memcpy(_best_frame, fdata, flen);
                    _best_len   = flen;
                    _best_w     = fw;
                    _best_h     = fh;
                    _best_fmt   = ffmt;
                    _best_score = person;
                }
            }
        }
    }

    bool enter_alert = false;
    bool send_best   = false;
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
                // Motion is only the wake-up: a person must be confirmed before alerting.
                _hits          = 0;
                _state         = ST_VERIFY;
                _state_ms      = now;
                _confirm_ms    = 0;
                _best_score    = -1;
                _best_len      = 0;
                _last_infer_ms = 0;
                mclog::tagInfo(TAG, "motion={}permille -> verifying person", mp);
            }
            break;
        }
        case ST_VERIFY: {
            if (person >= PERSON_THRESH_PCT && _confirm_ms == 0) {
                bool new_visit  = (_last_person_ms == 0) || (now - _last_person_ms >= REALERT_GAP_MS);
                _last_person_ms = now;
                if (!new_visit) {
                    // Same lingering visitor (seen < REALERT_GAP ago): stay silent and
                    // slide the quiet window. No alert, no photo, no Telegram.
                    mclog::tagInfo(TAG, "person again ({}%) within quiet window -> suppressed", person);
                    _state    = ST_COOLDOWN;
                    _state_ms = now;
                    break;
                }
                // First sighting of a new visit: fire the deterrence NOW; the photo
                // keeps improving for IMPROVE_MS more, then the best shot is sent.
                _confirm_ms = now;
                enter_alert = true;
                show_view(V_LABEL);
                set_main(TXT_WARN, 0xFFFFFF);
                set_alert_bg(true);
                mclog::tagInfo(TAG, "ALERT: person confirmed ({}%)", person);
            }
            if (_confirm_ms != 0) {
                if (now - _confirm_ms >= IMPROVE_MS || _best_score >= PERSON_GREAT_PCT) {
                    send_best = true;
                    _state    = ST_ALERTING;
                    _state_ms = _confirm_ms;  // alert window counts from confirmation
                }
            } else if (now - _state_ms > VERIFY_MS) {
                mclog::tagInfo(TAG, "no person within {}s -> rearm (no alert, no photo)",
                               (int)(VERIFY_MS / 1000));
                _state = ST_ARMED;
                _hits  = 0;
                _md.reset();
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
    }
    if (send_best) {
        send_best_photo();  // JPEG-encode the best person-frame; Telegram runs in its own task
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

    if (_best_frame) {
        heap_caps_free(_best_frame);
        _best_frame = nullptr;
        _best_len   = 0;
    }

    LvglLockGuard lock;
    if (_prev_scr) {
        lv_screen_load(_prev_scr);
        _prev_scr = nullptr;
    }
    _btn_quit.reset();
    if (_scr) {
        lv_obj_del(_scr);  // deletes the face/eyes/mouth/dot/label children too
        _scr = nullptr;
    }
    _label_main = nullptr;
    _face       = nullptr;
    _eye_l      = nullptr;
    _eye_r      = nullptr;
    _mouth      = nullptr;
    _dot        = nullptr;
}

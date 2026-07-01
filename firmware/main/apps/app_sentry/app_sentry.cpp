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
#include "jpg/image_to_jpeg.h"
#include <lvgl.h>
#include <cstdio>
#include <memory>
#include <string>

#include <board.h>
#include "esp_netif.h"
extern "C" {
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
}

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const char* TAG = "Sentry";

// Custom 48px font (app_sentry/sentry_font_48.c) with exactly the glyphs this screen
// shows. The stock font is a curated subset missing most of these characters.
LV_FONT_DECLARE(sentry_font_48);

// ---- Owner presence (WiFi) ----
// The sentry only arms while the owner's phone is NOT on the office WiFi.
// Set this to your phone's LAN IP (reserve it in the router for reliability).
static const char* OWNER_IP = "192.168.66.184";

// State messages (UTF-8). All glyphs are present in sentry_font_48.
static const char* TXT_ARM    = "\xE5\xB8\x83\xE9\x98\xB2";                          // 布防
static const char* TXT_GUARD  = "\xE7\x9B\x91\xE6\x8E\xA7\xE4\xB8\xAD";              // 监控中
static const char* TXT_WARN   = "\xE9\x9D\x9E\xE8\xAF\xB7\xE5\x8B\xBF\xE5\x8A\xA8";  // 非请勿动
static const char* TXT_COOL   = "\xE5\x86\xB7\xE5\x8D\xB4\xE4\xB8\xAD";              // 冷却中
static const char* TXT_DISARM = "\xE5\xB7\xB2\xE6\x92\xA4\xE9\x98\xB2";              // 已撤防
static const char* TXT_NOWIFI = "\xE7\xBD\x91\xE7\xBB\x9C\xE6\x96\xAD\xE5\xBC\x80";  // 网络断开

// ---- Detection tunables ----
static const int MOTION_TRIG_PERMILLE   = 40;
static const int MOTION_STRONG_PERMILLE = 150;
static const int CONSECUTIVE_HITS       = 2;
static const int PIX_THRESHOLD          = 18;
static const uint32_t GRACE_MS          = 5000;   // arming countdown (leave the frame)
static const uint32_t ALERT_MS          = 4000;
static const uint32_t COOLDOWN_MS       = 15000;
static const uint32_t CHECK_MS          = 100;

// ---- Presence tunables ----
static const uint32_t PROBE_MS        = 8000;   // how often we ARP-probe the phone
static const uint32_t AWAY_CONFIRM_MS = 60000;  // phone unseen this long -> "away" -> arm
                                                // (ARP cache lingers a few min, so real
                                                //  away-lag is a couple minutes; that's fine)

// ---- Head-shake tunables ----
static const int SHAKE_AMP          = 300;
static const int SHAKE_SPEED        = 900;
static const uint32_t SHAKE_HALF_MS = 150;
static const uint32_t LED_FLASH_MS  = 250;

enum SentryState { ST_NOWIFI, ST_DISARMED, ST_ARMING, ST_ARMED, ST_ALERTING, ST_COOLDOWN };
enum PresenceCond { C_NOWIFI, C_PRESENT, C_AWAY };
enum Presence { PR_NOWIFI, PR_SEEN, PR_UNSEEN };

static std::unique_ptr<Button> _btn_quit;
static lv_obj_t* _scr        = nullptr;
static lv_obj_t* _prev_scr   = nullptr;
static lv_obj_t* _label_main = nullptr;
static MotionDetector _md;
static SentryState _state;
static PresenceCond _cond;
static uint32_t _state_ms;
static uint32_t _last_check_ms;
static uint32_t _last_probe_ms;
static uint32_t _present_seen_ms;
static bool _wifi_ok;
static int _hits;
static int _shake_dir;
static uint32_t _shake_ms;

// --- ARP presence probe ---
struct ArpProbe {
    ip4_addr_t ip;
    bool wifi;
    bool found;
    bool gw_found;   // diagnostic: can we ARP our own gateway?
    uint32_t netif_ip;
};

// An ARP-capable, up netif with a real IP (the WiFi STA). MUST have NETIF_FLAG_ETHARP:
// calling etharp_* on the loopback netif (which is "up" with 127.0.0.1) asserts.
static bool netif_usable(struct netif* n)
{
    return n && netif_is_up(n) && (n->flags & NETIF_FLAG_ETHARP) &&
           !ip4_addr_isany_val(*netif_ip4_addr(n));
}

// Runs in the lwIP TCP/IP thread (via esp_netif_tcpip_exec) so etharp_* is safe.
static esp_err_t arp_probe_cb(void* ctx)
{
    ArpProbe* p       = static_cast<ArpProbe*>(ctx);
    struct netif* nif = netif_default;
    if (!netif_usable(nif)) {
        nif = nullptr;
        for (struct netif* n = netif_list; n != nullptr; n = n->next) {
            if (netif_usable(n)) {
                nif = n;
                break;
            }
        }
    }
    if (!nif) {
        p->wifi = false;
        return ESP_OK;
    }

    p->wifi     = true;
    p->netif_ip = netif_ip4_addr(nif)->addr;

    struct eth_addr* eth_ret = nullptr;
    const ip4_addr_t* ip_ret = nullptr;
    p->found                 = (etharp_find_addr(nif, &p->ip, &eth_ret, &ip_ret) >= 0);
    etharp_request(nif, &p->ip);  // refresh/renew for next probe

    // Diagnostic: is the gateway ARP-reachable? (proves our ARP path works at all)
    const ip4_addr_t* gw = netif_ip4_gw(nif);
    p->gw_found          = (etharp_find_addr(nif, gw, &eth_ret, &ip_ret) >= 0);
    etharp_request(nif, gw);
    return ESP_OK;
}

// WiFi is started once (async). WifiBoard::StartNetwork() is non-blocking — it kicks off
// the connect and returns; the ARP probe detects when the link is actually up. (The HAL's
// startNetwork() wrapper busy-waits until connected, which would freeze the mooncake loop.)
static bool _wifi_started = false;

static Presence probe_phone()
{
    static ArpProbe p;
    ip4addr_aton(OWNER_IP, &p.ip);
    p.wifi     = false;
    p.found    = false;
    p.gw_found = false;
    p.netif_ip = 0;
    esp_netif_tcpip_exec(arp_probe_cb, &p);
    uint32_t a = p.netif_ip;
    mclog::tagInfo(TAG, "probe: wifi={} phone={} gw={} netif={}.{}.{}.{}", p.wifi, p.found, p.gw_found,
                   (int)(a & 0xff), (int)((a >> 8) & 0xff), (int)((a >> 16) & 0xff), (int)((a >> 24) & 0xff));
    if (!p.wifi) return PR_NOWIFI;
    return p.found ? PR_SEEN : PR_UNSEEN;
}

// --- LVGL helpers (call under LvglLockGuard) ---
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

        _label_main = lv_label_create(_scr);
        lv_obj_set_style_text_font(_label_main, &sentry_font_48, 0);
        lv_obj_set_style_text_align(_label_main, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(_label_main, LV_ALIGN_CENTER, 0, -6);
        lv_obj_set_style_text_color(_label_main, lv_color_hex(0xFF8800), 0);
        lv_label_set_text(_label_main, TXT_NOWIFI);

        _btn_quit = std::make_unique<Button>(_scr);
        _btn_quit->setAlign(LV_ALIGN_BOTTOM_MID);
        _btn_quit->label().setText("EXIT");
        _btn_quit->onClick().connect([this]() { close(); });

        lv_screen_load(_scr);
    }

    // Keep the head still & under our control (no idle drift moving the camera).
    GetStackChan().motion().setModifyLock(true);

    // Bring WiFi up (needed for presence detection AND the future Telegram push).
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
    // WiFi is not up yet at boot -> start in NOWIFI. Once connected, assume the owner is
    // present (won't arm until the phone is actually confirmed absent).
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

    // (2) Pump the servo animation.
    GetStackChan().update();

    // (3) While alerting: blink LED + step head shake.
    if (_state == ST_ALERTING) {
        bool led_on = ((now - _state_ms) / LED_FLASH_MS) % 2 == 0;
        GetHAL().showRgbColor(led_on ? 255 : 0, 0, 0);
        if (now - _shake_ms >= SHAKE_HALF_MS) {
            _shake_ms  = now;
            _shake_dir = -_shake_dir;
            GetStackChan().motion().moveYawWithSpeed(_shake_dir * SHAKE_AMP, SHAKE_SPEED);
        }
    }

    // (4) Presence probe (throttled).
    if (now - _last_probe_ms >= PROBE_MS) {
        _last_probe_ms = now;
        Presence pr    = probe_phone();
        if (pr == PR_NOWIFI) {
            _wifi_ok = false;
        } else {
            _wifi_ok = true;
            if (pr == PR_SEEN) _present_seen_ms = now;
        }
    }

    // (5) Outer presence gate: NOWIFI / PRESENT(disarmed) / AWAY(run detection).
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
                set_main(TXT_NOWIFI, 0xFF8800);
            } else if (cond == C_PRESENT) {
                _state = ST_DISARMED;
                set_main(TXT_DISARM, 0x888888);
            } else {  // C_AWAY -> begin arming
                _state    = ST_ARMING;
                _state_ms = now;
                _hits     = 0;
                set_main(TXT_ARM, 0xFFAA00);
            }
        }
        if (cond == C_AWAY) {
            _md.reset();
        } else {
            GetHAL().showRgbColor(0, 0, 0);  // leaving armed flow: LEDs off
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
                set_main(TXT_GUARD, 0x33FF66);
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
                set_main(TXT_COOL, 0xAAAAAA);
            }
            break;
        }
        case ST_COOLDOWN: {
            if (now - _state_ms > COOLDOWN_MS) {
                _state = ST_ARMED;
                _md.reset();
                set_main(TXT_GUARD, 0x33FF66);
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
    _label_main = nullptr;
}

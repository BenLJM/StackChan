# StackChan Sentry Mode (as-built)

This fork adds a **presence-aware security sentry** app (`firmware/main/apps/app_sentry/`).
The StackChan guards a room **only while you're away**, warns intruders, and sends their
photo to your phone via Telegram. When you're home it behaves like a normal StackChan.

## Behaviour

| Owner state | How it's detected | Screen | Reacts to motion? |
|---|---|---|---|
| **Home** | your phone is on the office WiFi | StackChan face (blinking eyes) | No — disarmed |
| **Away** | phone absent from WiFi ~30 s | breathing red "sentry eye" | Yes — armed |
| **Intruder** | motion wakes it, then the **person classifier confirms a human** | red screen 「非请勿动」 + head-shake + red LED flash | Sends the **best person-frame** → Telegram |

Returning home (phone rejoins WiFi) disarms within ~5 s and stops any active alert.

**Alert fatigue (visit model)** — person sightings closer together than `REALERT_GAP_MS`
(5 min) count as one *visit*: only the first sighting fires the alert + Telegram; later
ones are silently logged and slide the window, so a colleague lingering near the camera
produces exactly one notification. The sentry eye breathes **orange** instead of red while
a visit is being suppressed. Re-arming (owner returns, then leaves again) resets this, and
a visitor who stays perfectly still for longer than the gap re-alerts on their next move.

## How it works

- **Presence** — a background `esp_ping` session pings the owner's phone (`OWNER_IP`). Each
  reply stamps a "last seen" time; no reply for `AWAY_CONFIRM_MS` (30 s) ⇒ away ⇒ arm.
  ICMP gives *fresh* liveness (an ARP cache lingers ~4 min, too slow). Immune to Android
  background-app-killing, unlike a BLE-beacon-app approach.
- **WiFi** — the mooncake/launcher boot path does **not** connect WiFi (only the xiaozhi AI
  path does), so the app calls `Board::GetInstance().StartNetwork()` (non-blocking) in
  `onOpen`, then reads `GetHAL().getWifiStatus()`.
- **Intruder detection (two-stage)** — cheap frame-difference motion (`motion_detect.{h,cpp}`,
  YUYV luma on a 40×30 grid) only *wakes* the pipeline. A VERIFY window (≤ `VERIFY_MS`) then
  runs the **TFLite-Micro `person_detect` classifier** (`person_detect.{h,cpp}` + embedded
  96×96 grayscale model, ~300 KB) every ~0.4 s: no person ⇒ silently re-arm (curtains, pets
  and light changes never alert); person confirmed ⇒ alert fires and the highest-scoring
  frame within `IMPROVE_MS` becomes the photo (**best shot**, not the motion-instant frame).
  The classifier is independent of esp-dl (whose face-detect box-decode is broken on this
  build); if the model fails to init the sentry degrades to motion-only alerts.
  Auto-arms on boot (`SENTRY_AUTOSTART`).
- **Alert** — head-shake via the servo (torque forced on; motion modifiers locked), RGB LED
  flash, on-screen warning, JPEG capture (`image_to_jpeg`), and a Telegram `sendPhoto`
  multipart upload run in a **background task** (24 KB stack for the TLS handshake — the main
  task is only 8 KB — so the UI never freezes). HTTPS uses the ESP-IDF cert bundle; system
  time comes from the RTC plus SNTP.

## Configuration

1. **Telegram** (optional): copy `firmware/main/apps/app_sentry/tg_secret.h.example` →
   `tg_secret.h` (git-ignored) and set `TG_BOT_TOKEN` (from @BotFather) and `TG_CHAT_ID`
   (from @userinfobot). If the file is absent, Telegram is compiled out and the rest works.
2. **Your phone's IP**: set `OWNER_IP` in `app_sentry.cpp`. Disable per-network MAC
   randomization on the phone (and/or reserve the IP in the router) so it stays stable.

## Build & flash (Windows / ESP-IDF 5.5.4)

```
python firmware/fetch_repos.py          # applies the xiaozhi patch with --ignore-whitespace
. C:\esp\esp-idf\export.ps1
idf.py -C <firmware> reconfigure        # only after ADDING source files (glob has no CONFIGURE_DEPENDS)
ninja -C <firmware>\build -j 4          # -j 4 avoids GCC ICE on high-core-count CPUs
idf.py -C <firmware> -p <PORT> flash
```

Build from a short path (long Windows paths overflow the command-line limit during link).

## Tunables (top of `app_sentry.cpp`)

`AWAY_CONFIRM_MS` (away lag), `MOTION_TRIG_PERMILLE` / `MOTION_STRONG_PERMILLE` /
`PIX_THRESHOLD` (motion sensitivity), `PERSON_THRESH_PCT` / `PERSON_GREAT_PCT` /
`VERIFY_MS` / `IMPROVE_MS` / `INFER_GAP_MS` (person verification & best-shot hunt),
`REALERT_GAP_MS` (alert-fatigue quiet window, default 5 min),
`GRACE_MS` / `ALERT_MS` / `COOLDOWN_MS` (timing), `SHAKE_AMP` / `SHAKE_SPEED` (head shake),
`PING_INTERVAL_MS`.

## Maintainer notes (non-obvious)

- **Mooncake has no foreground exclusivity** — every installed app draws to the shared
  `lv_screen_active()` each loop, and the launcher arms a DVD-logo screensaver after 30 s.
  A full-screen app must own its own screen (`lv_obj_create(NULL)` + `lv_screen_load`) and
  call `lv_display_trigger_activity(NULL)` every loop to suppress that screensaver.
- **The stock UI font (`font_puhui_basic_20_4`) is a glyph *subset*** missing most needed
  characters. The app ships a custom `sentry_font_48.c` generated with `lv_font_conv`.
- **Servo torque defaults OFF** and auto-releases after 200 ms; nothing enables it, so the
  head only moves if you `setTorqueEnabled(true)` first.
- **Opening the USB-Serial/JTAG port resets the ESP32-S3** (DTR/RTS) — you can't passively
  monitor without rebooting it.

## Key files

- `app_sentry/app_sentry.cpp` — app: presence gate + state machine + UI + alert + Telegram.
- `app_sentry/motion_detect.{h,cpp}` — stage 1: motion wake-up.
- `app_sentry/person_detect.{h,cpp}` + `person_detect.tflite` — stage 2: TFLM person
  classifier (model embedded via `EMBED_FILES`; needs `espressif/esp-tflite-micro`).
- `app_sentry/sentry_font_48.c` — custom Chinese font.
- `app_sentry/tg_secret.h.example` — Telegram credential template.

Design history (superseded by this doc): `docs/superpowers/specs/`.

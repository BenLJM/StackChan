# StackChan Sentry — WiFi Presence-Based Arming (Design)

Date: 2026-07-01
Status: implemented (app_sentry), pending live verification
Related: [2026-06-26-stackchan-sentry-design.md](2026-06-26-stackchan-sentry-design.md)

## Problem

The sentry triggers even when the owner is right next to it. The owner wants it to
**arm only when they are away** (out of the office) and stay disarmed while present.

## Decision

Detect owner presence by whether their **phone is on the office WiFi**, using a
**background ICMP ping** from the ESP32-S3 (no extra hardware; the device is already on WiFi).

- Chosen over BLE "phone as beacon": Android background-advertising is unreliable and
  China-market ROMs kill background apps → false "away" → false alarms. WiFi presence is
  maintained by the OS and is immune to app-killing.
- ARP was tried first but its cache entry *lingers* ~4 min after the phone leaves, so
  "away" took ~5 min — too slow. ICMP gives *fresh* liveness: away is detected in
  `AWAY_CONFIRM_MS` (~90 s). Requires the owner to disable per-network MAC randomization
  (so the phone keeps a stable IP); verified this phone answers ping even when idle.

## Behavior

Outer presence gate wraps the existing sentry state machine:

| Condition | Screen (48px, centered) | Detection |
|---|---|---|
| Phone on WiFi (present) | 「已撤防」gray | OFF — never triggers |
| Phone absent ≥ AWAY_CONFIRM (away) | 布防 N → 监控中 → 非请勿动 | ON — normal flow |
| Device itself offline | 「网络断开」amber | OFF (can't notify anyway) |

- Present → Away: phone unseen for `AWAY_CONFIRM_MS` (+ ARP cache expiry) → arm.
- Away → Present: phone seen again → **immediately** disarm (stops any in-progress alert,
  LEDs off, head recenters).
- Boot: assume present → start DISARMED (won't arm until proven away).

## Implementation (firmware/main/apps/app_sentry/app_sentry.cpp)

- `OWNER_IP` = phone's LAN IP (`192.168.66.184`; MAC randomization off → stable; reserve in
  router for extra safety, MAC `c0:2f:cd:59:12:d5`).
- A background `esp_ping` session (`PING_INTERVAL_MS` = 4s) pings the phone; the reply
  callback stamps `_present_seen_ms`. `_wifi_ok` comes from `GetHAL().getWifiStatus()`.
- `AWAY_CONFIRM_MS` = 90s → away ~1.5 min after the phone leaves; return is detected within
  one ping (~4s).
- WiFi must be brought up in the mooncake path (`Board::GetInstance().StartNetwork()`,
  non-blocking) — it is not connected there otherwise.
- Custom `sentry_font_48` provides the Chinese glyphs (stock font is a subset missing them).

## Out of scope / future

- Telegram `/arm` `/disarm` manual override (folds into the pending Telegram push, B3).
- DHCP reservation guidance for the phone IP.
- Optional dedicated BLE beacon if WiFi presence proves insufficient.

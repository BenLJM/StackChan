# StackChan Sentry — WiFi Presence-Based Arming (Design)

Date: 2026-07-01
Status: implemented (app_sentry), pending live verification
Related: [2026-06-26-stackchan-sentry-design.md](2026-06-26-stackchan-sentry-design.md)

## Problem

The sentry triggers even when the owner is right next to it. The owner wants it to
**arm only when they are away** (out of the office) and stay disarmed while present.

## Decision

Detect owner presence by whether their **phone is on the office WiFi**, using an
**ARP probe** from the ESP32-S3 (no extra hardware; the device is already on WiFi).

- Chosen over BLE "phone as beacon": Android background-advertising is unreliable and
  China-market ROMs kill background apps → false "away" → false alarms. WiFi presence is
  maintained by the OS and is immune to app-killing.
- Chosen over ICMP ping: a present phone's ARP entry *lingers*, so ARP **never**
  false-reports "away" while the phone is on WiFi → no false alarms while present (the
  exact complaint). Trade-off: "away" detection lags a few minutes (ARP cache expiry),
  which is fine for "I left the office".

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

- `OWNER_IP` = phone's LAN IP (currently `192.168.66.199`; reserve in router for stability).
- `probe_phone()` runs `etharp_find_addr` + `etharp_request` inside `esp_netif_tcpip_exec`
  (thread-safe lwIP access; core-locking is off). Uses `netif_default` (the STA).
  Returns SEEN / UNSEEN / NOWIFI.
- Probe every `PROBE_MS` (15s); `AWAY_CONFIRM_MS` = 60s (real lag ~2–4 min w/ ARP aging).
- Custom `sentry_font_48` provides the Chinese glyphs (stock font is a subset missing them).

## Out of scope / future

- Telegram `/arm` `/disarm` manual override (folds into the pending Telegram push, B3).
- DHCP reservation guidance for the phone IP.
- Optional dedicated BLE beacon if WiFi presence proves insufficient.

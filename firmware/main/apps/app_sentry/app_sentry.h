/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>

/**
 * @brief Sentry app: camera + on-device face detection.
 *
 * Phase 1: capture frames, run esp-dl human face detection, and log the
 * detected face boxes / score / area-fraction to the serial monitor so we can
 * prove detection works before wiring up the alert + Telegram pipeline.
 */
class AppSentry : public mooncake::AppAbility {
public:
    AppSentry();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

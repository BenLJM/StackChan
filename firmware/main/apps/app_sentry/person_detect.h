/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>

/**
 * @brief "Is there a person in this frame?" classifier.
 *
 * Wraps the official TFLite-Micro person_detect model (96x96 grayscale MobileNet,
 * embedded in the app image). This is a CLASSIFIER, not a box detector — it is
 * independent of esp-dl, whose detection box-decode is broken on this build.
 */

/// Load the model + allocate the tensor arena (PSRAM). Idempotent; returns false
/// if the model/arena setup failed (then person_detect_score() returns -1).
bool person_detect_init();

/// Run one inference on a YUYV(YUY2) frame. Blocks ~0.3-0.6 s.
/// @return person confidence in percent 0..100, or -1 if unavailable/error.
int person_detect_score(const uint8_t* yuyv, int w, int h);

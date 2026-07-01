/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <cstddef>

/**
 * @brief Lightweight frame-difference motion detector for YUYV camera frames.
 *
 * Downsamples the luma (Y) channel into a small grid and counts how many cells
 * changed by more than a threshold versus the previous frame. Robust to sensor
 * noise (downsampling averages it out) and cheap (no AI model). Used as the
 * Sentry trigger after the esp-dl face detector proved unusable on this build.
 */
class MotionDetector {
public:
    static constexpr int COLS = 40;
    static constexpr int ROWS = 30;
    static constexpr int CELLS = COLS * ROWS;

    // Process one YUYV frame (w*h*2 bytes). Returns false if the frame is invalid.
    bool update(const uint8_t* yuyv, int w, int h);

    // Fraction of grid cells that changed since the previous frame, in permille (0..1000).
    int motionPermille() const { return _motionPermille; }

    bool hasPrev() const { return _hasPrev; }
    void reset() { _hasPrev = false; _motionPermille = 0; }

    // Per-cell luma-difference threshold (0..255) to count a cell as "changed".
    int pixThreshold = 22;

private:
    uint8_t _prev[CELLS];
    uint8_t _cur[CELLS];
    bool _hasPrev       = false;
    int _motionPermille = 0;
};

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "motion_detect.h"

// YUYV layout: bytes are [Y0,U,Y1,V] -> luma of pixel x is at byte (y*w + x)*2.
bool MotionDetector::update(const uint8_t* yuyv, int w, int h)
{
    if (!yuyv || w <= 0 || h <= 0) {
        return false;
    }
    const int cellW = w / COLS;
    const int cellH = h / ROWS;
    if (cellW < 1 || cellH < 1) {
        return false;
    }

    // Downsample luma into the COLS x ROWS grid (sample every 2nd pixel for speed).
    for (int cy = 0; cy < ROWS; cy++) {
        for (int cx = 0; cx < COLS; cx++) {
            uint32_t sum = 0;
            int cnt      = 0;
            const int y0 = cy * cellH;
            const int x0 = cx * cellW;
            for (int yy = y0; yy < y0 + cellH; yy += 2) {
                const uint8_t* row = yuyv + (size_t)yy * w * 2;
                for (int xx = x0; xx < x0 + cellW; xx += 2) {
                    sum += row[xx * 2];  // Y of pixel xx
                    cnt++;
                }
            }
            _cur[cy * COLS + cx] = cnt ? (uint8_t)(sum / cnt) : 0;
        }
    }

    if (_hasPrev) {
        int changed = 0;
        for (int i = 0; i < CELLS; i++) {
            int d = (int)_cur[i] - (int)_prev[i];
            if (d < 0) d = -d;
            if (d > pixThreshold) changed++;
        }
        _motionPermille = changed * 1000 / CELLS;
    } else {
        _motionPermille = 0;
    }

    for (int i = 0; i < CELLS; i++) {
        _prev[i] = _cur[i];
    }
    _hasPrev = true;
    return true;
}

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

// Boot-time self-test for remote verification (no touch / no human needed):
// spawns a task that runs the esp-dl face detector on live camera frames a few
// times and logs the results to the serial console, then exits. Guarded by the
// SENTRY_SELFTEST compile definition so it is excluded from normal builds.
void sentry_selftest_run();

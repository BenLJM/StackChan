/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "person_detect.h"
#include <esp_heap_caps.h>
#include <mooncake_log.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Embedded model (main/CMakeLists.txt EMBED_FILES apps/app_sentry/person_detect.tflite).
extern const uint8_t person_detect_tflite_start[] asm("_binary_person_detect_tflite_start");

static const char* TAG = "PersonDetect";

static const int ARENA_SIZE = 200 * 1024;  // example uses 136 KB; leave headroom (PSRAM)

static bool _ready  = false;
static bool _failed = false;
static tflite::MicroInterpreter* _interpreter = nullptr;
static TfLiteTensor* _input                   = nullptr;

bool person_detect_init()
{
    if (_ready) return true;
    if (_failed) return false;

    const tflite::Model* model = tflite::GetModel(person_detect_tflite_start);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        mclog::tagWarn(TAG, "model schema {} != {}", (int)model->version(), TFLITE_SCHEMA_VERSION);
        _failed = true;
        return false;
    }

    uint8_t* arena = (uint8_t*)heap_caps_malloc(ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena) {
        mclog::tagWarn(TAG, "arena alloc failed ({} bytes)", ARENA_SIZE);
        _failed = true;
        return false;
    }

    // Ops used by the person_detect model (same set as the upstream example).
    static tflite::MicroMutableOpResolver<5> resolver;
    resolver.AddAveragePool2D();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddReshape();
    resolver.AddSoftmax();

    static tflite::MicroInterpreter interpreter(model, resolver, arena, ARENA_SIZE);
    _interpreter = &interpreter;

    if (_interpreter->AllocateTensors() != kTfLiteOk) {
        mclog::tagWarn(TAG, "AllocateTensors failed");
        heap_caps_free(arena);
        _interpreter = nullptr;
        _failed      = true;
        return false;
    }

    _input = _interpreter->input(0);
    if (!_input || _input->type != kTfLiteInt8 || _input->dims->size != 4) {
        mclog::tagWarn(TAG, "unexpected input tensor");
        _failed = true;
        return false;
    }

    mclog::tagInfo(TAG, "ready: input {}x{} arena used {}/{} bytes", (int)_input->dims->data[1],
                   (int)_input->dims->data[2], (int)_interpreter->arena_used_bytes(), ARENA_SIZE);
    _ready = true;
    return true;
}

int person_detect_score(const uint8_t* yuyv, int w, int h)
{
    if (!_ready || !yuyv || w <= 0 || h <= 0) return -1;

    const int rows = _input->dims->data[1];  // 96
    const int cols = _input->dims->data[2];  // 96

    // Downscale YUYV luma (Y at byte (y*w+x)*2) to rows x cols, uint8 -> int8.
    int8_t* dst = _input->data.int8;
    for (int y = 0; y < rows; y++) {
        const uint8_t* src_row = yuyv + (size_t)(y * h / rows) * w * 2;
        for (int x = 0; x < cols; x++) {
            dst[y * cols + x] = (int8_t)((int)src_row[(x * w / cols) * 2] - 128);
        }
    }

    if (_interpreter->Invoke() != kTfLiteOk) {
        mclog::tagWarn(TAG, "Invoke failed");
        return -1;
    }

    TfLiteTensor* out = _interpreter->output(0);
    if (!out || out->type != kTfLiteInt8) return -1;
    // Output: int8 softmax scores [not_person, person] (person index = 1).
    int8_t person = out->data.int8[1];
    float pct     = ((float)person - (float)out->params.zero_point) * out->params.scale * 100.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (int)pct;
}

#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* 每项自检的结果：ok 表示通过，detail 为附加说明（可为空串）。 */
struct selftest_result
{
    bool ok;
    char detail[32];
};

void selftest_leds(struct selftest_result *out);     /* 5 颗按键灯逐个点亮 + 底部灯 */
void selftest_psram(struct selftest_result *out);    /* PSRAM 容量 */
void selftest_microphone(struct selftest_result *out); /* 采集一段 PCM */
void selftest_wifi(struct selftest_result *out);     /* 经板载 C2 扫描 Wi-Fi */

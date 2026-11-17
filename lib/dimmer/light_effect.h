// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
// Light effect described as a cycle: fade up -> hold high -> fade down -> hold low
#pragma once

#include <stdint.h>

struct LightEffect
{
    uint8_t id;              // Zigbee Identify Trigger Effect ID
    const char* name;        // for log/serial
    uint16_t cycles;         // number of cycles, 0 = endless (until stopped or maxDurationMs)
    uint16_t riseMs;         // low -> high (0 = jump)
    uint16_t highMs;         // hold high
    uint16_t fallMs;         // high -> low (0 = jump)
    uint16_t lowMs;          // hold low
    uint8_t lowLevel;        // lower brightness (Zigbee level, 0 = off)
    uint8_t highLevel;       // upper brightness (Zigbee level)
    uint32_t maxDurationMs;  // safety limit, 0 = none
};

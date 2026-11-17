// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
// Central configuration: pins, I2C, timings, LW18 parameters
#pragma once

#include <stdint.h>

#include "light_effect.h"

namespace config
{

// ---------- Version ----------
constexpr const char* firmwareVersion = "1.0.0.0";  // Zigbee Basic swBuildId, shown by Z2M as firmware version
constexpr const char* firmwareDate = "20261117";    // Zigbee Basic dateCode (YYYYMMDD)

// ---------- Pins (XIAO ESP32-C6) ----------
constexpr uint8_t buttonCount = 4;
constexpr uint8_t pinButton[buttonCount] = {
    0,   // button 1: D0 = GPIO0
    1,   // button 2: D1 = GPIO1
    2,   // button 3: D2 = GPIO2
    21,  // button 4: D3 = GPIO21
};

constexpr uint8_t pinSda = 22;        // D4
constexpr uint8_t pinScl = 23;        // D5
constexpr uint8_t pinBootButton = 9;  // onboard BOOT button (factory reset)
constexpr uint8_t pinStatusLed = 15;  // onboard, active LOW
constexpr bool statusLedActiveLow = true;

// ---------- Serial interface ----------
constexpr uint32_t serialBaud = 115200;
constexpr uint32_t serialWaitMs = 3000;  // max. time to wait for USB CDC at startup
// Serial command console (scan, read, pwm, fx, zb, ...) for setup and diagnostics.
// false = commands are not compiled in (about 4 KB less flash); log output stays active.
constexpr bool enableSerialConsole = true;

// ---------- I2C / LED-Warrior18 ----------
constexpr uint32_t i2cFrequency = 100000;  // 100 kHz, conservative because of the level shifter
constexpr uint16_t i2cTimeoutMs = 20;
constexpr uint8_t lw18Address = 0x20;       // 7 bit default address
constexpr uint8_t lw18Retries = 2;          // additional attempts after an I2C error
constexpr uint32_t lw18RetryMinMs = 5000;   // LW18 not reachable: first retry after 5 s ...
constexpr uint32_t lw18RetryMaxMs = 60000;  // ... then doubled up to max. 60 s
constexpr uint32_t lw18CheckMs = 10000;     // read back the PWM register (detects a LW18 restart)

// PWM period: 48 MHz / (n + 1) -> n = 16383 gives about 2.9 kHz at 14 bit
constexpr uint16_t lw18PeriodReg = 16383;
// Value for 100 % (PWM value >= period -> 100 %)
constexpr uint16_t lw18PwmMax = lw18PeriodReg + 1;

// ---------- Buttons ----------
constexpr uint16_t buttonDebounceMs = 50;
constexpr uint16_t buttonDoubleClickMs = 300;  // time to wait for a double click
constexpr uint16_t buttonLongPressMs = 600;    // "hold" from here on
constexpr uint16_t factoryResetHoldMs = 3000;  // hold the BOOT button this long -> factory reset

// ---------- Local control ----------
enum class LocalAction : uint8_t
{
    none,    // no local action
    toggle,  // toggle on/off
    full,    // 100 %
    dim,     // dim while held, direction changes with every hold
};

// Light channels a button controls (always both in sync mode)
enum class ChannelSelect : uint8_t
{
    ch1,
    ch2,
    both,
};

struct ButtonActions
{
    LocalAction single;
    LocalAction doubleClick;
    LocalAction hold;  // applies to hold + release
    ChannelSelect channels;
};

constexpr ButtonActions localActions[buttonCount] = {
    {LocalAction::toggle, LocalAction::full, LocalAction::dim, ChannelSelect::both},  // button 1
    {LocalAction::none, LocalAction::none, LocalAction::none, ChannelSelect::both},   // button 2
    {LocalAction::none, LocalAction::none, LocalAction::none, ChannelSelect::both},   // button 3
    {LocalAction::none, LocalAction::none, LocalAction::none, ChannelSelect::both},   // button 4
};

constexpr bool localControlDefault = true;  // default after first start
constexpr bool syncDefault = true;          // channels in sync, default after first start

// ---------- NVS ----------
constexpr const char* nvsNamespace = "lw18dim";
constexpr const char* nvsKeyLocalControl = "local";
constexpr const char* nvsKeyFadeTime = "fade";  // OnOffTransitionTime in 1/10 s
constexpr const char* nvsKeyLightOn = "on";     // channel 1
constexpr const char* nvsKeyLightLevel = "level";
constexpr const char* nvsKeyLight2On = "on2";  // channel 2
constexpr const char* nvsKeyLight2Level = "level2";
constexpr const char* nvsKeySync = "sync";
constexpr const char* nvsKeyStartUp = "startup";  // StartUpOnOff (ZCL value)
constexpr uint32_t nvsSaveDelayMs = 5000;         // save on/off + level only 5 s after the last change

// ---------- State after power failure ----------
constexpr uint8_t startUpOnOffDefault = 0xFF;  // ZCL StartUpOnOff: 0 off, 1 on, 2 toggle, 0xFF previous
constexpr bool lightOnDefault = false;         // first start: off ...
constexpr uint8_t lightLevelDefault = 254;     // ... with full brightness when switched on the first time

// ---------- Zigbee ----------
constexpr uint8_t zigbeeEndpointLight = 10;
constexpr uint8_t zigbeeEndpointButtonFirst = 11;  // buttons 1..4 -> endpoints 11..14
constexpr uint8_t zigbeeEndpointLocal = 15;        // switch "local control"
constexpr uint8_t zigbeeEndpointLight2 = 16;       // light channel 2 (endpoint 10 = channel 1)
constexpr uint8_t zigbeeEndpointSync = 17;         // switch "channels in sync"

// Multistate input PresentValue per button event (0 = idle, ignored by the converter)
enum class ButtonEvent : uint16_t
{
    idle = 0,
    single = 1,
    doubleClick = 2,
    hold = 3,
    release = 4,
};
constexpr uint16_t buttonEventStateCount = 5;  // 0..4
constexpr const char* zigbeeManufacturer = "DIY";
constexpr const char* zigbeeModel = "LW18-Dimmer-4T";  // must match the Z2M converter
constexpr uint8_t zigbeeLevelMax = 254;
// Reports go directly to the coordinator (Z2M), independent of binding/reporting configuration
constexpr uint16_t zigbeeCoordinatorAddress = 0x0000;
constexpr uint8_t zigbeeCoordinatorEndpoint = 1;
constexpr uint32_t zigbeeReportIntervalMs = 50;  // minimum gap between two reports (stack buffers)
// Report the state of all channels/switches periodically, even without a change: reports get lost while
// the coordinator/Z2M is down, Z2M shows the correct state again after this time at the latest
constexpr uint32_t zigbeeStateReportMs = 300000;  // 5 min

// ---------- Dimmer ----------
constexpr float dimmerGamma = 2.2f;        // brightness curve level -> PWM
constexpr uint16_t dimmerFadeMs = 400;     // default fade for on/off/level (changeable via Zigbee)
constexpr uint16_t dimmerFadeMaxDs = 600;  // upper limit OnOffTransitionTime: 60 s (in 1/10 s)
constexpr uint32_t attributePollMs = 500;  // poll OnOffTransitionTime/StartUpOnOff this often
// The Zigbee stack executes transitions (OnOffTransitionTime, move to level) itself in ~100 ms steps;
// the dimmer only smooths between these steps
constexpr uint16_t zigbeeStepSmoothMs = 100;
// A longer gap between level updates = a new transition starts (the level before is remembered)
constexpr uint32_t zigbeeTransitionGapMs = 250;
constexpr uint8_t dimmerLevelMin = 3;            // minimum brightness when dimming with a button
constexpr uint16_t dimmerFullRangeMs = 4000;     // hold: 0 -> 100 % in this time
constexpr uint16_t dimmerOutputIntervalMs = 10;  // max. ~100 I2C writes/s while fading

// ---------- Light effects (Zigbee Identify -> Trigger Effect) ----------
// IDs 0x00-0x0B as defined by the ZCL, own effects from 0x10. Names must match the Z2M converter.
// {id, name, cycles (0 = endless), rise ms, high ms, fall ms, low ms, low level, high level, max. ms}
// clang-format off
constexpr LightEffect lightEffects[] = {
    {0x00, "blink",          1,  0,    500, 0,    500,  0, 254, 0},
    {0x01, "breathe",        15, 500,  0,   500,  0,    0, 254, 0},
    {0x02, "okay",           2,  0,    250, 0,    250,  0, 254, 0},
    {0x0B, "channel_change", 1,  0,    500, 0,    7500, 1, 254, 0},
    {0x10, "pulse_fast",     5,  1000, 0,   500,  0,    0, 254, 0},
    {0x11, "pulse_slow",     5,  2000, 0,   2000, 0,    0, 254, 0},
    {0x12, "alarm",          0,  0,    250, 0,    250,  0, 254, 300000},  // endless, safety stop after 5 min
};
// clang-format on
constexpr uint8_t effectIdFinish = 0xFE;  // finish the current cycle, then stop
constexpr uint8_t effectIdStop = 0xFF;    // stop immediately

// ---------- Status LED ----------
constexpr uint16_t statusLedBlinkMs = 500;  // blink interval while not joined

}  // namespace config

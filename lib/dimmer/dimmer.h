// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
// Brightness logic: gamma curve, smooth fading, dimming with a button
#pragma once

#include <stdint.h>

#include "light_effect.h"

struct DimmerConfig
{
    uint16_t pwmMax;            // PWM value for 100 %
    uint8_t levelMax;           // highest Zigbee level (254)
    uint8_t levelMin;           // minimum brightness when dimming with a button
    float gamma;                // exponent of the brightness curve
    uint16_t fadeMs;            // fade time for on/off and level changes
    uint16_t dimFullRangeMs;    // time for 0 -> levelMax while holding
    uint16_t outputIntervalMs;  // minimum gap between PWM outputs while fading/dimming
};

class Dimmer
{
public:
    // Writes a PWM value to the hardware. nullptr = the caller reads pwm() itself.
    using OutputFunction = void (*)(uint16_t pwm);

    Dimmer(const DimmerConfig& config, OutputFunction output);

    // Set the initial state without fading and output it immediately
    void begin(bool on, uint8_t level);

    // Set the state; level 0 = keep the last brightness. Fades with fadeMs from the configuration.
    // During an effect: switching off ends the effect, otherwise only the state after the effect
    // is updated (HA additionally sends "ON" when starting an effect).
    void set(bool on, uint8_t level);
    // Like above, but with its own fade time (e.g. only smoothing the steps of the Zigbee stack)
    void set(bool on, uint8_t level, uint16_t fadeMs);

    // Start an effect (replaces a running one). Returns to the previous state afterwards.
    void startEffect(const LightEffect& effect);
    // Let the current cycle finish, then stop
    void finishEffect();
    // Stop immediately
    void stopEffect();
    bool isEffectRunning() const { return m_effectActive; }
    const char* effectName() const { return m_effectActive ? m_effect.name : ""; }

    // Continuous dimming while held (off + up -> starts at the minimum brightness)
    void startDimming(bool up);
    void stopDimming();

    // Non-blocking, call regularly from loop()
    void loop();

    bool isOn() const { return m_on; }
    uint8_t level() const { return m_level; }
    uint16_t pwm() const { return m_lastPwm; }
    bool isBusy() const { return m_fading || m_dimming || m_effectActive; }
    bool isDimming() const { return m_dimming; }

    // Change the default fade time at runtime (0 = switch instantly), applies from the next fade
    void setFadeMs(uint16_t fadeMs) { m_config.fadeMs = fadeMs; }
    uint16_t fadeMs() const { return m_config.fadeMs; }

    uint16_t levelToPwm(float level) const;

private:
    void startFade(uint16_t fadeMs);
    // Returns false when the effect has ended
    bool updateEffect(uint32_t now);
    void endEffect();
    void writeOutput(uint32_t now);

    DimmerConfig m_config;
    OutputFunction m_output;

    bool m_on = false;
    uint8_t m_level;         // last brightness (1..levelMax), also while off
    float m_current = 0.0f;  // current brightness in level units (0 = off)

    bool m_fading = false;
    float m_fadeFrom = 0.0f;
    float m_fadeTo = 0.0f;
    uint32_t m_fadeStart = 0;
    uint16_t m_fadeDurationMs = 0;  // duration of the running fade

    bool m_dimming = false;
    bool m_dimUp = true;
    uint32_t m_dimLast = 0;

    bool m_effectActive = false;
    LightEffect m_effect = {};
    uint32_t m_effectStart = 0;
    bool m_effectFinishing = false;
    uint32_t m_effectLastCycle = 0;  // with finishEffect: last cycle that still runs

    uint16_t m_lastPwm = 0;
    uint32_t m_lastOutput = 0;
};

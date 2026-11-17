// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
#include "dimmer.h"

#include <Arduino.h>
#include <math.h>

Dimmer::Dimmer(const DimmerConfig& config, OutputFunction output)
    : m_config(config), m_output(output), m_level(config.levelMax)
{
}

void Dimmer::begin(bool on, uint8_t level)
{
    if (level > 0)
    {
        m_level = min(level, m_config.levelMax);
    }
    m_on = on;
    m_current = m_on ? m_level : 0.0f;
    m_fading = false;
    m_dimming = false;
    m_lastPwm = levelToPwm(m_current);
    m_lastOutput = millis();
    if (m_output != nullptr)
    {
        m_output(m_lastPwm);
    }
}

void Dimmer::set(bool on, uint8_t level)
{
    set(on, level, m_config.fadeMs);
}

void Dimmer::set(bool on, uint8_t level, uint16_t fadeMs)
{
    if (level > 0)
    {
        m_level = min(level, m_config.levelMax);
    }
    if (m_effectActive)
    {
        if (on)
        {
            m_on = true;  // applies after the effect
            return;
        }
        m_effectActive = false;  // switching off ends the effect, fade from the current effect brightness
    }
    m_on = on;
    m_dimming = false;
    startFade(fadeMs);
}

void Dimmer::startEffect(const LightEffect& effect)
{
    uint32_t cycleMs = static_cast<uint32_t>(effect.riseMs) + effect.highMs + effect.fallMs + effect.lowMs;
    if (cycleMs == 0)
    {
        return;
    }
    m_effect = effect;
    m_effectActive = true;
    m_effectFinishing = false;
    m_effectStart = millis();
    m_fading = false;
    m_dimming = false;
}

void Dimmer::finishEffect()
{
    if (!m_effectActive || m_effectFinishing)
    {
        return;
    }
    uint32_t cycleMs = static_cast<uint32_t>(m_effect.riseMs) + m_effect.highMs + m_effect.fallMs + m_effect.lowMs;
    m_effectFinishing = true;
    m_effectLastCycle = (millis() - m_effectStart) / cycleMs;
}

void Dimmer::stopEffect()
{
    if (m_effectActive)
    {
        endEffect();
    }
}

void Dimmer::endEffect()
{
    m_effectActive = false;
    m_effectFinishing = false;
    startFade(m_config.fadeMs);  // smoothly back to the state before the effect
}

bool Dimmer::updateEffect(uint32_t now)
{
    uint32_t elapsed = now - m_effectStart;
    uint32_t cycleMs = static_cast<uint32_t>(m_effect.riseMs) + m_effect.highMs + m_effect.fallMs + m_effect.lowMs;
    uint32_t cycle = elapsed / cycleMs;

    bool cyclesDone = (m_effect.cycles > 0 && cycle >= m_effect.cycles);
    bool finished = (m_effectFinishing && cycle > m_effectLastCycle);
    bool timeout = (m_effect.maxDurationMs > 0 && elapsed >= m_effect.maxDurationMs);
    if (cyclesDone || finished || timeout)
    {
        return false;
    }

    float low = m_effect.lowLevel;
    float high = m_effect.highLevel;
    uint32_t position = elapsed % cycleMs;
    if (position < m_effect.riseMs)
    {
        m_current = low + (high - low) * position / m_effect.riseMs;
        return true;
    }
    position -= m_effect.riseMs;
    if (position < m_effect.highMs)
    {
        m_current = high;
        return true;
    }
    position -= m_effect.highMs;
    if (position < m_effect.fallMs)
    {
        m_current = high - (high - low) * position / m_effect.fallMs;
        return true;
    }
    m_current = low;
    return true;
}

void Dimmer::startDimming(bool up)
{
    stopEffect();
    if (!m_on)
    {
        if (!up)
        {
            return;
        }
        m_on = true;
    }
    m_fading = false;
    if (m_current < m_config.levelMin)
    {
        m_current = m_config.levelMin;
    }
    m_dimming = true;
    m_dimUp = up;
    m_dimLast = millis();
}

void Dimmer::stopDimming()
{
    if (!m_dimming)
    {
        return;
    }
    m_dimming = false;
    m_level = static_cast<uint8_t>(constrain(lroundf(m_current), 1L, static_cast<long>(m_config.levelMax)));
    m_current = m_level;
}

void Dimmer::loop()
{
    uint32_t now = millis();

    if (m_effectActive)
    {
        if (!updateEffect(now))
        {
            endEffect();
        }
    }

    if (m_effectActive)
    {
        // the effect has set m_current
    }
    else if (m_dimming)
    {
        uint32_t elapsed = now - m_dimLast;
        m_dimLast = now;
        float step = static_cast<float>(elapsed) * m_config.levelMax / m_config.dimFullRangeMs;
        m_current += m_dimUp ? step : -step;
        m_current = constrain(m_current, static_cast<float>(m_config.levelMin), static_cast<float>(m_config.levelMax));
        m_level = static_cast<uint8_t>(lroundf(m_current));
    }
    else if (m_fading)
    {
        uint32_t elapsed = now - m_fadeStart;
        if (elapsed >= m_fadeDurationMs)
        {
            m_current = m_fadeTo;
            m_fading = false;
        }
        else
        {
            float progress = static_cast<float>(elapsed) / m_fadeDurationMs;
            m_current = m_fadeFrom + (m_fadeTo - m_fadeFrom) * progress;
        }
    }

    writeOutput(now);
}

uint16_t Dimmer::levelToPwm(float level) const
{
    if (level <= 0.0f)
    {
        return 0;
    }
    if (level >= m_config.levelMax)
    {
        return m_config.pwmMax;
    }
    float ratio = level / m_config.levelMax;
    uint16_t pwm = static_cast<uint16_t>(m_config.pwmMax * powf(ratio, m_config.gamma) + 0.5f);
    // every level > 0 must be visibly on
    return pwm < 1 ? 1 : pwm;
}

void Dimmer::startFade(uint16_t fadeMs)
{
    float target = m_on ? static_cast<float>(m_level) : 0.0f;
    if (fadeMs == 0)
    {
        m_current = target;
        m_fading = false;
        return;
    }
    m_fadeDurationMs = fadeMs;
    m_fadeFrom = m_current;
    m_fadeTo = target;
    m_fadeStart = millis();
    m_fading = (m_fadeFrom != m_fadeTo);
}

void Dimmer::writeOutput(uint32_t now)
{
    uint16_t pwm = levelToPwm(m_current);
    if (pwm == m_lastPwm)
    {
        return;
    }
    // rate limited while fading/dimming, the final value is always written immediately
    if (isBusy() && (now - m_lastOutput) < m_config.outputIntervalMs)
    {
        return;
    }
    m_lastPwm = pwm;
    m_lastOutput = now;
    if (m_output != nullptr)
    {
        m_output(pwm);
    }
}

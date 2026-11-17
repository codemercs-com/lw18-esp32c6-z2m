// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
#include "light_channel.h"

#include <Arduino.h>

#include "zigbee_report.h"

LightChannel::LightChannel(const LightChannelConfig& config, const DimmerConfig& dimmerConfig, Lw18Light& zigbee)
    : m_config(config), m_dimmer(dimmerConfig, nullptr), m_zigbee(zigbee)
{
}

void LightChannel::onZigbeeChange(bool state, uint8_t level)
{
    portENTER_CRITICAL(&m_mux);
    m_pendingState = state;
    m_pendingLevel = level;
    m_changePending = true;
    portEXIT_CRITICAL(&m_mux);
}

void LightChannel::onZigbeeEffect(uint8_t effectId)
{
    portENTER_CRITICAL(&m_mux);
    m_pendingEffect = effectId;
    m_effectPending = true;
    portEXIT_CRITICAL(&m_mux);
}

bool LightChannel::takeZigbeeChange(bool& state, uint8_t& level)
{
    portENTER_CRITICAL(&m_mux);
    bool pending = m_changePending;
    state = m_pendingState;
    level = m_pendingLevel;
    m_changePending = false;
    portEXIT_CRITICAL(&m_mux);
    return pending;
}

bool LightChannel::takeEffect(uint8_t& effectId)
{
    portENTER_CRITICAL(&m_mux);
    bool pending = m_effectPending;
    effectId = m_pendingEffect;
    m_effectPending = false;
    portEXIT_CRITICAL(&m_mux);
    return pending;
}

bool LightChannel::isEcho(bool state, uint8_t level) const
{
    return state == m_dimmer.isOn() && (level == 0 || level == m_dimmer.level());
}

uint8_t LightChannel::resolveTargetLevel(bool state, uint8_t level, uint32_t transitionGapMs)
{
    uint32_t now = millis();
    // A longer gap since the last update = a new transition starts
    if (now - m_lastZigbeeUpdate > transitionGapMs)
    {
        m_levelBeforeTransition = m_dimmer.level();
    }
    m_lastZigbeeUpdate = now;
    return (!state && m_levelBeforeTransition > 0) ? m_levelBeforeTransition : level;
}

void LightChannel::reportNow()
{
    m_reportPending = false;
    m_zigbee.setLight(m_dimmer.isOn(), m_dimmer.level());
    uint8_t endpoint = m_zigbee.getEndpoint();
    reportAttribute(endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    reportAttribute(endpoint, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
}

void LightChannel::syncZigbee()
{
    m_zigbee.setLight(m_dimmer.isOn(), m_dimmer.level());
    m_reportPending = true;
}

void LightChannel::reportIfDue(uint32_t settleMs)
{
    // Report only when the dimmer is idle and the Zigbee stack no longer sends transition steps
    if (m_reportPending && !m_dimmer.isBusy() && millis() - m_lastZigbeeUpdate > settleMs)
    {
        reportNow();
    }
}

void LightChannel::loadState(Preferences& preferences, bool defaultOn, uint8_t defaultLevel, uint8_t levelMax)
{
    m_savedOn = preferences.getBool(m_config.nvsKeyOn, defaultOn);
    m_savedLevel = preferences.getUChar(m_config.nvsKeyLevel, defaultLevel);
    if (m_savedLevel == 0 || m_savedLevel > levelMax)
    {
        m_savedLevel = defaultLevel;
    }
    m_observedOn = m_savedOn;
    m_observedLevel = m_savedLevel;
}

void LightChannel::saveStateIfDue(Preferences& preferences, uint32_t delayMs)
{
    uint32_t now = millis();
    bool on = m_dimmer.isOn();
    uint8_t level = m_dimmer.level();
    if (on != m_observedOn || level != m_observedLevel || m_dimmer.isBusy())
    {
        m_observedOn = on;
        m_observedLevel = level;
        m_stateChangedAt = now;
        return;
    }
    if ((on == m_savedOn && level == m_savedLevel) || now - m_stateChangedAt < delayMs)
    {
        return;
    }
    if (on != m_savedOn)
    {
        preferences.putBool(m_config.nvsKeyOn, on);
    }
    if (level != m_savedLevel)
    {
        preferences.putUChar(m_config.nvsKeyLevel, level);
    }
    m_savedOn = on;
    m_savedLevel = level;
    Serial.printf("NVS %s: saved, light %s, level=%u\n", m_config.name, on ? "ON" : "OFF", level);
}

void LightChannel::logIfDone()
{
    if (m_dimmer.isBusy() || m_dimmer.pwm() == m_loggedPwm)
    {
        return;
    }
    m_loggedPwm = m_dimmer.pwm();
    Serial.printf("Dimmer %s: %s, level=%u, PWM=%u\n", m_config.name, m_dimmer.isOn() ? "ON" : "OFF", m_dimmer.level(),
                  m_dimmer.pwm());
}

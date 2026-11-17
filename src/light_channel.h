// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
// One light channel of the LW18: dimmer + Zigbee endpoint + state stored in NVS
#pragma once

#include <Preferences.h>

#include "dimmer.h"
#include "lw18_light.h"

struct LightChannelConfig
{
    const char* name;         // for the log, e.g. "K1"
    const char* nvsKeyOn;     // NVS key on/off
    const char* nvsKeyLevel;  // NVS key brightness
};

class LightChannel
{
public:
    LightChannel(const LightChannelConfig& config, const DimmerConfig& dimmerConfig, Lw18Light& zigbee);

    const char* name() const { return m_config.name; }
    Dimmer& dimmer() { return m_dimmer; }
    Lw18Light& zigbee() { return m_zigbee; }

    // ----- Called from the Zigbee task: only store -----
    void onZigbeeChange(bool state, uint8_t level);
    void onZigbeeEffect(uint8_t effectId);

    // ----- Called from loop() -----
    bool takeZigbeeChange(bool& state, uint8_t& level);
    bool takeEffect(uint8_t& effectId);

    // Detect the echo of an own report (otherwise a running fade restarts)
    bool isEcho(bool state, uint8_t level) const;

    // Target brightness for a Zigbee value. When switching off, the stack fades down in steps;
    // these steps must not overwrite the remembered brightness.
    uint8_t resolveTargetLevel(bool state, uint8_t level, uint32_t transitionGapMs);

    // Copy the state to Zigbee and report it immediately
    void reportNow();
    // Copy the state to Zigbee, report it once the dimmer is idle. Also used for the commanded channel:
    // Z2M stores e.g. 75 % optimistically as brightness 191.25, the report corrects it to 191.
    void syncZigbee();
    // Only schedule a report; the attributes in the stack are already correct (command came via Zigbee)
    void markReport() { m_reportPending = true; }
    void reportIfDue(uint32_t settleMs);

    void loadState(Preferences& preferences, bool defaultOn, uint8_t defaultLevel, uint8_t levelMax);
    bool savedOn() const { return m_savedOn; }
    uint8_t savedLevel() const { return m_savedLevel; }
    // Save on/off + level only when nothing has changed for delayMs
    void saveStateIfDue(Preferences& preferences, uint32_t delayMs);

    // Log the final value after fade/dimming/effect
    void logIfDone();

private:
    LightChannelConfig m_config;
    Dimmer m_dimmer;
    Lw18Light& m_zigbee;

    portMUX_TYPE m_mux = portMUX_INITIALIZER_UNLOCKED;
    bool m_changePending = false;
    bool m_pendingState = false;
    uint8_t m_pendingLevel = 0;
    bool m_effectPending = false;
    uint8_t m_pendingEffect = 0;

    uint32_t m_lastZigbeeUpdate = 0;
    uint8_t m_levelBeforeTransition = 0;
    bool m_reportPending = false;

    bool m_savedOn = false;
    uint8_t m_savedLevel = 0;
    bool m_observedOn = false;
    uint8_t m_observedLevel = 0;
    uint32_t m_stateChangedAt = 0;

    uint16_t m_loggedPwm = 0;
};

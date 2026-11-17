// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
// Dimmable Zigbee light with additional attributes:
//  - Level Control 0x0010 OnOffTransitionTime (fade time, 1/10 s)
//  - On/Off 0x4003 StartUpOnOff (behavior after a power failure)
// The Arduino library does not forward write accesses to these attributes,
// so they are polled from loop().
#pragma once

#include <Zigbee.h>

// StartUpOnOff values as defined by the ZCL
enum class StartUpOnOff : uint8_t
{
    off = 0x00,
    on = 0x01,
    toggle = 0x02,
    previous = 0xFF,
};

class Lw18Light : public ZigbeeDimmableLight
{
public:
    // Runs in the Zigbee task: only store the values, execute in loop()
    using EffectCallback = void (*)(uint8_t effectId, uint8_t effectVariant);

    explicit Lw18Light(uint8_t endpoint);

    // Intercept Identify -> Trigger Effect (the Arduino library does not forward it).
    // Call after Zigbee.begin(); up to maxInstances endpoints.
    void onTriggerEffect(EffectCallback callback);

    // Call all add...() before Zigbee.begin()
    // Firmware version (Basic swBuildId) and date (Basic dateCode), max. 16 characters
    bool setSoftwareVersion(const char* version, const char* date);
    bool addTransitionTimeAttribute(uint16_t transitionTime);
    bool addStartUpOnOffAttribute(StartUpOnOff startUpOnOff);

    bool readTransitionTime(uint16_t& transitionTime);
    bool writeTransitionTime(uint16_t transitionTime);
    bool readStartUpOnOff(StartUpOnOff& startUpOnOff);
    bool writeStartUpOnOff(StartUpOnOff startUpOnOff);

    static bool isValidStartUpOnOff(uint8_t value);

private:
    static constexpr uint8_t maxInstances = 4;

    static bool rawCommandHandler(uint8_t bufferId);
    static Lw18Light* findInstance(uint8_t endpoint);
    static Lw18Light* m_instances[maxInstances];
    static uint8_t m_instanceCount;

    EffectCallback m_effectCallback = nullptr;

    static constexpr size_t zclStringMax = 16;
    // Add or replace a ZCL string (length byte + characters) in the Basic cluster
    bool setBasicString(uint16_t attributeId, const char* text, char* buffer);

    char m_versionZcl[zclStringMax + 2] = {};
    char m_dateZcl[zclStringMax + 2] = {};

    bool readAttribute(uint16_t clusterId, uint16_t attributeId, void* value, size_t size);
    bool writeAttribute(uint16_t clusterId, uint16_t attributeId, void* value);

    // Initial values passed when the attributes are created
    uint16_t m_transitionTime = 0;
    uint8_t m_startUpOnOff = static_cast<uint8_t>(StartUpOnOff::previous);
};

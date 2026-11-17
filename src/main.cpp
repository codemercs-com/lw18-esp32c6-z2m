// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
// LW18 dimmer: 2 light channels (EP 10 / 16), button events (EP 11-14),
// switches "local control" (EP 15) and "channels in sync" (EP 17).
// State in NVS, StartUpOnOff, light effects, LW18 supervision with I2C recovery, serial test commands
#ifndef ZIGBEE_MODE_ZCZR
#error "ZIGBEE_MODE_ZCZR muss gesetzt sein (platformio.ini)"
#endif

#include <Arduino.h>
#include <Button2.h>
#include <Preferences.h>
#include <Wire.h>
#include <Zigbee.h>
#include <esp_log.h>

#include "config.h"
#include "dimmer.h"
#include "i2c_bus_recovery.h"
#include "light_channel.h"
#include "lw18.h"
#include "lw18_light.h"
#include "zigbee_report.h"

namespace
{

constexpr uint8_t channelCount = 2;

constexpr DimmerConfig dimmerConfig = {
    config::lw18PwmMax,   config::zigbeeLevelMax,    config::dimmerLevelMin,         config::dimmerGamma,
    config::dimmerFadeMs, config::dimmerFullRangeMs, config::dimmerOutputIntervalMs,
};

Lw18 lw18(Wire, config::lw18Address, config::lw18Retries);
Button2 buttons[config::buttonCount];
Button2 bootButton;
// Channel 1 holds the shared settings fade time and StartUpOnOff (fade time mirrored to channel 2)
Lw18Light zbLight1(config::zigbeeEndpointLight);
Lw18Light zbLight2(config::zigbeeEndpointLight2);
ZigbeeMultistate zbButtons[config::buttonCount] = {
    ZigbeeMultistate(config::zigbeeEndpointButtonFirst),
    ZigbeeMultistate(config::zigbeeEndpointButtonFirst + 1),
    ZigbeeMultistate(config::zigbeeEndpointButtonFirst + 2),
    ZigbeeMultistate(config::zigbeeEndpointButtonFirst + 3),
};
static_assert(config::buttonCount == 4, "adapt zbButtons to buttonCount");
ZigbeeLight zbLocal(config::zigbeeEndpointLocal);
ZigbeeLight zbSync(config::zigbeeEndpointSync);
Preferences preferences;

LightChannel channels[channelCount] = {
    LightChannel({"K1", config::nvsKeyLightOn, config::nvsKeyLightLevel}, dimmerConfig, zbLight1),
    LightChannel({"K2", config::nvsKeyLight2On, config::nvsKeyLight2Level}, dimmerConfig, zbLight2),
};

String inputLine;
bool lw18Ready = false;
bool localControl = config::localControlDefault;
bool syncChannels = config::syncDefault;
bool dimUpNext = true;  // dim direction for the next hold

// Switch states from the Zigbee task, applied in loop()
portMUX_TYPE switchMux = portMUX_INITIALIZER_UNLOCKED;
bool localPending = false;
bool localState = false;
bool syncPending = false;
bool syncState = false;

uint16_t fadeTimeDs = config::dimmerFadeMs / 100;  // OnOffTransitionTime in 1/10 s
StartUpOnOff startUpOnOff = static_cast<StartUpOnOff>(config::startUpOnOffDefault);
uint32_t attributeLastPoll = 0;

// LW18 output and supervision
uint16_t writtenPwm[channelCount] = {0, 0};
bool outputValid = false;  // false = write in any case on the next pass
uint32_t lw18LastAttempt = 0;
uint32_t lw18RetryMs = config::lw18RetryMinMs;
uint32_t lw18LastCheck = 0;

bool zigbeeConnectedLast = false;
uint32_t stateReportLast = 0;
uint32_t statusLedLastToggle = 0;
bool statusLedOn = false;

// ---------- Helpers ----------

void setStatusLed(bool on)
{
    bool level = config::statusLedActiveLow ? !on : on;
    digitalWrite(config::pinStatusLed, level ? HIGH : LOW);
}

uint16_t percentToPwm(float percent)
{
    if (percent <= 0.0f)
    {
        return 0;
    }
    if (percent >= 100.0f)
    {
        return config::lw18PwmMax;
    }
    return static_cast<uint16_t>(percent * config::lw18PwmMax / 100.0f + 0.5f);
}

bool anyChannelBusy()
{
    for (LightChannel& channel : channels)
    {
        if (channel.dimmer().isBusy())
        {
            return true;
        }
    }
    return false;
}

// Is the channel part of the selection? Always all channels in sync mode.
bool isSelected(uint8_t index, config::ChannelSelect select)
{
    if (syncChannels || select == config::ChannelSelect::both)
    {
        return true;
    }
    return (select == config::ChannelSelect::ch1 && index == 0) || (select == config::ChannelSelect::ch2 && index == 1);
}

void scanI2c()
{
    Serial.println("I2C scan ...");
    uint8_t found = 0;
    for (uint8_t address = 1; address < 127; ++address)
    {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0)
        {
            Serial.printf("  device at 0x%02X\n", address);
            ++found;
        }
    }
    Serial.printf("I2C scan done, %u device(s) found\n", found);
}

void printSignature()
{
    Lw18Signature signature;
    if (!lw18.readSignature(signature))
    {
        Serial.println("Reading the signature failed (LW18 connected?)");
        return;
    }
    Serial.printf("Vendor=%u Product=%u Version=%X.%X.%X.%X\n", signature.vendor, signature.product,
                  (signature.version >> 12) & 0xF, (signature.version >> 8) & 0xF, (signature.version >> 4) & 0xF,
                  signature.version & 0xF);
}

void printPwm()
{
    uint16_t ch1 = 0;
    uint16_t ch2 = 0;
    if (!lw18.readPwm16(ch1, ch2))
    {
        Serial.println("Reading PWM failed");
        return;
    }
    Serial.printf("PWM1=%u PWM2=%u (100%% = %u)\n", ch1, ch2, config::lw18PwmMax);
}

void setPwm(uint16_t ch1, uint16_t ch2)
{
    if (lw18.setPwm16(ch1, ch2))
    {
        Serial.printf("PWM set: CH1=%u CH2=%u\n", ch1, ch2);
    }
    else
    {
        Serial.println("Setting PWM failed");
    }
}

void printHelp()
{
    Serial.println("Commands:");
    Serial.println("  help            this help");
    Serial.println("  scan            scan the I2C bus");
    Serial.println("  init            free the I2C bus, re-initialize the LW18");
    Serial.println("  sig             read the signature");
    Serial.println("  read            read the PWM registers");
    Serial.println("  pwm <a> [b]     set raw values (0..period+1), b = a if empty");
    Serial.println("  pct <a> [b]     set percent (0..100), b = a if empty");
    Serial.println("  per <n>         set the period (48 MHz / (n+1))");
    Serial.println("  dim <1|-1|0>    dim all channels up / down / stop (like holding a button)");
    Serial.println("  fade [ds]       show / set the fade time (1/10 s, 0 = instant)");
    Serial.println("  fx [name] [c]   start an effect (c = 1/2, empty = all) / list (stop, finish)");
    Serial.println("  zb              Zigbee and light status");
    Serial.println("  zbreset         Zigbee factory reset (leaves the network, restart)");
}

// ---------- LW18 output and supervision ----------

void markLw18Failed()
{
    lw18Ready = false;
    lw18RetryMs = config::lw18RetryMinMs;
    lw18LastAttempt = millis() - lw18RetryMs;  // retry immediately
}

// Write both channels in one I2C transfer, only on change
void updateOutput()
{
    if (!lw18Ready)
    {
        return;  // done after a successful re-initialization
    }
    uint16_t pwm1 = channels[0].dimmer().pwm();
    uint16_t pwm2 = channels[1].dimmer().pwm();
    if (outputValid && pwm1 == writtenPwm[0] && pwm2 == writtenPwm[1])
    {
        return;
    }
    if (!lw18.setPwm16(pwm1, pwm2))
    {
        Serial.println("LW18: write error, re-initializing the bus");
        markLw18Failed();
        return;
    }
    writtenPwm[0] = pwm1;
    writtenPwm[1] = pwm2;
    outputValid = true;
}

// Free the bus, re-initialize the LW18; the output follows in updateOutput()
bool reinitLw18()
{
    bool busFree = recoverI2cBus(Wire, config::pinSda, config::pinScl, config::i2cFrequency, config::i2cTimeoutMs);
    if (!busFree)
    {
        Serial.println("I2C: bus not free after recovery (SDA/SCL LOW)");
    }
    lw18Ready = lw18.begin(config::lw18PeriodReg);
    outputValid = false;
    return lw18Ready;
}

// Not reachable -> retry with a growing interval.
// Reachable -> read back the PWM registers; a difference = LW18 restarted (period lost).
void superviseLw18()
{
    uint32_t now = millis();
    if (!lw18Ready)
    {
        if (now - lw18LastAttempt < lw18RetryMs)
        {
            return;
        }
        lw18LastAttempt = now;
        if (reinitLw18())
        {
            Serial.println("LW18: ready again");
            lw18RetryMs = config::lw18RetryMinMs;
            lw18LastCheck = now;
        }
        else
        {
            lw18RetryMs = min(lw18RetryMs * 2, config::lw18RetryMaxMs);
            Serial.printf("LW18: not reachable, next attempt in %lu s\n",
                          static_cast<unsigned long>(lw18RetryMs / 1000));
        }
        return;
    }

    if (!outputValid || anyChannelBusy() || now - lw18LastCheck < config::lw18CheckMs)
    {
        return;
    }
    lw18LastCheck = now;
    uint16_t ch1 = 0;
    uint16_t ch2 = 0;
    bool readOk = lw18.readPwm16(ch1, ch2);
    bool pwmOk = readOk && ch1 == writtenPwm[0] && ch2 == writtenPwm[1];
    // The period is write only: set it again as a precaution. If the LW18 restarts while the
    // light is off (PWM 0 = read back matches), it would otherwise be lost unnoticed.
    if (pwmOk && lw18.setPeriod(config::lw18PeriodReg, config::lw18PeriodReg))
    {
        return;
    }
    if (pwmOk)
    {
        Serial.println("LW18: error writing the period, re-initializing");
    }
    else if (readOk)
    {
        Serial.printf("LW18: PWM %u/%u instead of %u/%u (restart?), re-initializing\n", ch1, ch2, writtenPwm[0],
                      writtenPwm[1]);
    }
    else
    {
        Serial.println("LW18: read error, re-initializing");
    }
    markLw18Failed();
}

// ---------- Zigbee callbacks (run in the Zigbee task: only store) ----------

void onLight1Change(bool state, uint8_t level)
{
    channels[0].onZigbeeChange(state, level);
}

void onLight2Change(bool state, uint8_t level)
{
    channels[1].onZigbeeChange(state, level);
}

void onLight1Effect(uint8_t effectId, uint8_t /*effectVariant*/)
{
    channels[0].onZigbeeEffect(effectId);
}

void onLight2Effect(uint8_t effectId, uint8_t /*effectVariant*/)
{
    channels[1].onZigbeeEffect(effectId);
}

void onLocalChange(bool state)
{
    portENTER_CRITICAL(&switchMux);
    localState = state;
    localPending = true;
    portEXIT_CRITICAL(&switchMux);
}

void onSyncChange(bool state)
{
    portENTER_CRITICAL(&switchMux);
    syncState = state;
    syncPending = true;
    portEXIT_CRITICAL(&switchMux);
}

// ---------- Reporting to Zigbee ----------

// Every event is reported, also the same value twice in a row
void reportButtonEvent(uint8_t index, config::ButtonEvent event)
{
    ZigbeeMultistate& zbButton = zbButtons[index];
    zbButton.setMultistateInput(static_cast<uint16_t>(event));
    reportAttribute(config::zigbeeEndpointButtonFirst + index, ESP_ZB_ZCL_CLUSTER_ID_MULTI_INPUT,
                    ESP_ZB_ZCL_ATTR_MULTI_INPUT_PRESENT_VALUE_ID);
}

void reportSwitch(ZigbeeLight& zbSwitch, bool state)
{
    zbSwitch.setLight(state);
    reportAttribute(zbSwitch.getEndpoint(), ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
}

// ---------- Lights: Zigbee commands ----------

void applyLight(uint8_t index)
{
    LightChannel& source = channels[index];
    bool state = false;
    uint8_t level = 0;
    if (!source.takeZigbeeChange(state, level) || source.isEcho(state, level))
    {
        return;
    }

    Serial.printf("Zigbee %s: light %s, level=%u\n", source.name(), state ? "ON" : "OFF", level);
    uint8_t targetLevel = source.resolveTargetLevel(state, level, config::zigbeeTransitionGapMs);
    // The Zigbee stack does the fade (intermediate steps), only smooth here; fade time 0 -> instant
    uint16_t smoothMs = fadeTimeDs == 0 ? 0 : config::zigbeeStepSmoothMs;

    for (uint8_t i = 0; i < channelCount; ++i)
    {
        if (i != index && !syncChannels)
        {
            continue;
        }
        LightChannel& channel = channels[i];
        channel.dimmer().set(state, targetLevel, smoothMs);
        // Report when the fade is done: the other channel follows in sync mode,
        // the commanded channel corrects the optimistic Z2M value (decimal places)
        if (i == index)
        {
            channel.markReport();
        }
        else
        {
            channel.syncZigbee();
        }
    }
    // Z2M switches off with level 0, and switching on first sends level 0:
    // the dimmer keeps the last brightness, correct the Zigbee attribute accordingly
    if (state && level == 0)
    {
        source.reportNow();
    }
}

// ---------- Light effects ----------

const LightEffect* findEffect(uint8_t effectId)
{
    for (const LightEffect& effect : config::lightEffects)
    {
        if (effect.id == effectId)
        {
            return &effect;
        }
    }
    return nullptr;
}

const LightEffect* findEffect(const char* name)
{
    for (const LightEffect& effect : config::lightEffects)
    {
        if (strcmp(effect.name, name) == 0)
        {
            return &effect;
        }
    }
    return nullptr;
}

void runEffect(LightChannel& channel, uint8_t effectId)
{
    Dimmer& dimmer = channel.dimmer();
    if (effectId == config::effectIdStop)
    {
        Serial.printf("Effect %s: stop\n", channel.name());
        dimmer.stopEffect();
        return;
    }
    if (effectId == config::effectIdFinish)
    {
        Serial.printf("Effect %s: finish\n", channel.name());
        dimmer.finishEffect();
        return;
    }
    const LightEffect* effect = findEffect(effectId);
    if (effect == nullptr)
    {
        Serial.printf("Effect %s: unknown ID 0x%02X\n", channel.name(), effectId);
        return;
    }
    Serial.printf("Effect %s: %s\n", channel.name(), effect->name);
    dimmer.startEffect(*effect);
}

// Effect on the source channel, on all channels in sync mode
void runEffectFrom(uint8_t index, uint8_t effectId)
{
    for (uint8_t i = 0; i < channelCount; ++i)
    {
        if (i == index || syncChannels)
        {
            runEffect(channels[i], effectId);
        }
    }
}

void applyEffects()
{
    for (uint8_t i = 0; i < channelCount; ++i)
    {
        uint8_t effectId = 0;
        if (channels[i].takeEffect(effectId))
        {
            runEffectFrom(i, effectId);
        }
    }
}

// ---------- Switches: local control (EP 15), channels in sync (EP 17) ----------

void applySwitches()
{
    portENTER_CRITICAL(&switchMux);
    bool local = localPending;
    bool localValue = localState;
    bool sync = syncPending;
    bool syncValue = syncState;
    localPending = false;
    syncPending = false;
    portEXIT_CRITICAL(&switchMux);

    if (local && localValue != localControl)
    {
        localControl = localValue;
        // Rare change (only via Zigbee) -> save immediately
        preferences.putBool(config::nvsKeyLocalControl, localControl);
        Serial.printf("Local control: %s\n", localControl ? "ON" : "OFF");
    }

    if (sync && syncValue != syncChannels)
    {
        syncChannels = syncValue;
        preferences.putBool(config::nvsKeySync, syncChannels);
        Serial.printf("Channels in sync: %s\n", syncChannels ? "ON" : "OFF");
        if (syncChannels)
        {
            // Channel 2 takes over the state of channel 1
            Dimmer& master = channels[0].dimmer();
            channels[1].dimmer().set(master.isOn(), master.level());
            channels[1].syncZigbee();
        }
    }
}

// ---------- Fade time and StartUpOnOff (shared, attributes on channel 1) ----------

void applyFadeTime(uint16_t transitionTime)
{
    fadeTimeDs = transitionTime;
    for (LightChannel& channel : channels)
    {
        channel.dimmer().setFadeMs(static_cast<uint16_t>(transitionTime * 100));
    }
}

const char* startUpOnOffName(StartUpOnOff value)
{
    switch (value)
    {
        case StartUpOnOff::off:
            return "off";
        case StartUpOnOff::on:
            return "on";
        case StartUpOnOff::toggle:
            return "toggle";
        case StartUpOnOff::previous:
            return "previous";
    }
    return "?";
}

void pollFadeTime()
{
    uint16_t transitionTime = 0;
    if (!zbLight1.readTransitionTime(transitionTime) || transitionTime == fadeTimeDs)
    {
        return;
    }
    if (transitionTime > config::dimmerFadeMaxDs)
    {
        transitionTime = config::dimmerFadeMaxDs;
        zbLight1.writeTransitionTime(transitionTime);
    }
    applyFadeTime(transitionTime);
    // Channel 2 needs the attribute too: the Zigbee stack fades on/off per endpoint itself
    zbLight2.writeTransitionTime(transitionTime);
    preferences.putUShort(config::nvsKeyFadeTime, fadeTimeDs);
    Serial.printf("Fade time: %u ms\n", fadeTimeDs * 100);
}

void pollStartUpOnOff()
{
    StartUpOnOff value = startUpOnOff;
    if (!zbLight1.readStartUpOnOff(value) || value == startUpOnOff)
    {
        return;
    }
    if (!Lw18Light::isValidStartUpOnOff(static_cast<uint8_t>(value)))
    {
        zbLight1.writeStartUpOnOff(startUpOnOff);  // invalid -> restore the old value
        return;
    }
    startUpOnOff = value;
    preferences.putUChar(config::nvsKeyStartUp, static_cast<uint8_t>(startUpOnOff));
    Serial.printf("Power on behavior: %s\n", startUpOnOffName(startUpOnOff));
}

// Apply changes via Zigbee (write attribute from Z2M); the library does not report them
void pollZigbeeAttributes()
{
    uint32_t now = millis();
    if (now - attributeLastPoll < config::attributePollMs)
    {
        return;
    }
    attributeLastPoll = now;
    pollFadeTime();
    pollStartUpOnOff();
}

// ---------- State at startup ----------

// On/off after startup according to StartUpOnOff
bool startUpState(bool previousOn)
{
    switch (startUpOnOff)
    {
        case StartUpOnOff::off:
            return false;
        case StartUpOnOff::on:
            return true;
        case StartUpOnOff::toggle:
            return !previousOn;
        case StartUpOnOff::previous:
            return previousOn;
    }
    return previousOn;
}

void loadSettings()
{
    localControl = preferences.getBool(config::nvsKeyLocalControl, config::localControlDefault);
    syncChannels = preferences.getBool(config::nvsKeySync, config::syncDefault);

    uint8_t storedStartUp = preferences.getUChar(config::nvsKeyStartUp, config::startUpOnOffDefault);
    startUpOnOff = Lw18Light::isValidStartUpOnOff(storedStartUp)
                       ? static_cast<StartUpOnOff>(storedStartUp)
                       : static_cast<StartUpOnOff>(config::startUpOnOffDefault);

    uint16_t storedFadeTime = preferences.getUShort(config::nvsKeyFadeTime, config::dimmerFadeMs / 100);
    applyFadeTime(min(storedFadeTime, config::dimmerFadeMaxDs));

    Serial.printf("NVS: local control %s, sync %s, power on behavior: %s, fade time: %u ms\n",
                  localControl ? "ON" : "OFF", syncChannels ? "ON" : "OFF", startUpOnOffName(startUpOnOff),
                  fadeTimeDs * 100);
}

void startChannels()
{
    for (LightChannel& channel : channels)
    {
        channel.loadState(preferences, config::lightOnDefault, config::lightLevelDefault, config::zigbeeLevelMax);
        bool startOn = startUpState(channel.savedOn());
        channel.dimmer().begin(startOn, channel.savedLevel());
        Serial.printf("Start %s: light %s, level=%u\n", channel.name(), startOn ? "ON" : "OFF", channel.savedLevel());
    }
}

// ---------- Status ----------

void reportAllStates()
{
    for (LightChannel& channel : channels)
    {
        // Report the brightness only here (final value), not every fade step from the stack
        disableAutoReporting(channel.zigbee().getEndpoint(), ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                             ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
        channel.reportNow();
    }
    reportSwitch(zbLocal, localControl);
    reportSwitch(zbSync, syncChannels);
    stateReportLast = millis();
}

// Report regularly so Z2M is correct again after an outage of the coordinator/Z2M
void reportStatesPeriodically()
{
    if (!Zigbee.connected() || anyChannelBusy() || millis() - stateReportLast < config::zigbeeStateReportMs)
    {
        return;
    }
    reportAllStates();
}

void updateStatusLed()
{
    bool connected = Zigbee.connected();
    if (connected != zigbeeConnectedLast)
    {
        zigbeeConnectedLast = connected;
        Serial.println(connected ? "Zigbee: joined the network" : "Zigbee: not connected");
        if (connected)
        {
            statusLedOn = false;
            setStatusLed(false);
            // Report the current state once so Z2M/HA are correct after startup
            reportAllStates();
        }
    }
    if (connected)
    {
        return;
    }
    uint32_t now = millis();
    if (now - statusLedLastToggle >= config::statusLedBlinkMs)
    {
        statusLedLastToggle = now;
        statusLedOn = !statusLedOn;
        setStatusLed(statusLedOn);
    }
}

void printZigbeeStatus()
{
    Serial.printf(
        "Zigbee: started=%s connected=%s, local control %s, sync %s, uptime %lu s, "
        "last reset: %d\n",
        Zigbee.started() ? "yes" : "no", Zigbee.connected() ? "yes" : "no", localControl ? "ON" : "OFF",
        syncChannels ? "ON" : "OFF", static_cast<unsigned long>(millis() / 1000), static_cast<int>(esp_reset_reason()));
    for (LightChannel& channel : channels)
    {
        Dimmer& dimmer = channel.dimmer();
        Serial.printf("  %s: light %s, level=%u, PWM=%u, Zigbee %s/%u%s%s\n", channel.name(),
                      dimmer.isOn() ? "ON" : "OFF", dimmer.level(), dimmer.pwm(),
                      channel.zigbee().getLightState() ? "ON" : "OFF", channel.zigbee().getLightLevel(),
                      dimmer.isEffectRunning() ? ", effect " : "", dimmer.effectName());
    }
}

void factoryReset()
{
    Serial.println("Zigbee factory reset, restarting ...");
    Serial.flush();
    Zigbee.factoryReset();
}

void setupZigbee()
{
    zbLight1.setManufacturerAndModel(config::zigbeeManufacturer, config::zigbeeModel);
    zbLight1.setPowerSource(ZB_POWER_SOURCE_MAINS);
    if (!zbLight1.setSoftwareVersion(config::firmwareVersion, config::firmwareDate))
    {
        Serial.println("Could not set the firmware version in the Basic cluster");
    }
    zbLight1.onLightChange(onLight1Change);
    if (!zbLight1.addTransitionTimeAttribute(fadeTimeDs))
    {
        Serial.println("Could not create attribute OnOffTransitionTime");
    }
    if (!zbLight1.addStartUpOnOffAttribute(startUpOnOff))
    {
        Serial.println("Could not create attribute StartUpOnOff");
    }
    Zigbee.addEndpoint(&zbLight1);

    for (uint8_t i = 0; i < config::buttonCount; ++i)
    {
        ZigbeeMultistate& zbButton = zbButtons[i];
        char description[16];
        snprintf(description, sizeof(description), "Button %u", i + 1);
        zbButton.addMultistateInput();
        zbButton.setMultistateInputApplication(ZB_MULTISTATE_APPLICATION_TYPE_OTHER_INDEX);
        zbButton.setMultistateInputDescription(description);
        zbButton.setMultistateInputStates(config::buttonEventStateCount);
        Zigbee.addEndpoint(&zbButton);
    }

    zbLocal.onLightChange(onLocalChange);
    Zigbee.addEndpoint(&zbLocal);

    zbLight2.onLightChange(onLight2Change);
    // Shared fade time: set on channel 1, channel 2 follows (pollFadeTime)
    if (!zbLight2.addTransitionTimeAttribute(fadeTimeDs))
    {
        Serial.println("Could not create attribute OnOffTransitionTime (channel 2)");
    }
    Zigbee.addEndpoint(&zbLight2);

    zbSync.onLightChange(onSyncChange);
    Zigbee.addEndpoint(&zbSync);

    if (!Zigbee.begin(ZIGBEE_ROUTER))
    {
        Serial.println("Zigbee start failed, restarting ...");
        Serial.flush();
        ESP.restart();
    }
    zbLight1.onTriggerEffect(onLight1Effect);
    zbLight2.onTriggerEffect(onLight2Effect);
    Serial.println("Zigbee started (router), searching network - permit join in Z2M");
}

// ---------- Serial commands ----------

void handleEffectCommand(const String& line)
{
    char name[24] = {0};
    int channelNumber = 0;
    int count = sscanf(line.c_str(), "%*s %23s %d", name, &channelNumber);
    if (count < 1)
    {
        Serial.print("Effects:");
        for (const LightEffect& effect : config::lightEffects)
        {
            Serial.printf(" %s", effect.name);
        }
        Serial.println(" stop finish");
        return;
    }
    uint8_t effectId = 0;
    const LightEffect* effect = findEffect(name);
    if (effect != nullptr)
    {
        effectId = effect->id;
    }
    else if (strcmp(name, "stop") == 0)
    {
        effectId = config::effectIdStop;
    }
    else if (strcmp(name, "finish") == 0)
    {
        effectId = config::effectIdFinish;
    }
    else
    {
        Serial.printf("Unknown effect '%s'\n", name);
        return;
    }
    for (uint8_t i = 0; i < channelCount; ++i)
    {
        if (count < 2 || channelNumber == i + 1)
        {
            runEffect(channels[i], effectId);
        }
    }
}

void handleCommand(const String& line)
{
    char command[8] = {0};
    float first = 0.0f;
    float second = 0.0f;
    int count = sscanf(line.c_str(), "%7s %f %f", command, &first, &second);
    if (count < 1)
    {
        return;
    }
    if (count == 2)
    {
        second = first;
    }

    String cmd(command);
    cmd.toLowerCase();

    if (cmd == "help")
    {
        printHelp();
    }
    else if (cmd == "scan")
    {
        scanI2c();
    }
    else if (cmd == "init")
    {
        Serial.println(reinitLw18() ? "LW18 ready" : "LW18 not ready");
    }
    else if (cmd == "sig")
    {
        printSignature();
    }
    else if (cmd == "read")
    {
        printPwm();
    }
    else if (cmd == "pwm" && count >= 2)
    {
        setPwm(static_cast<uint16_t>(constrain(first, 0.0f, 65535.0f)),
               static_cast<uint16_t>(constrain(second, 0.0f, 65535.0f)));
    }
    else if (cmd == "pct" && count >= 2)
    {
        setPwm(percentToPwm(first), percentToPwm(second));
    }
    else if (cmd == "per" && count >= 2)
    {
        uint16_t period = static_cast<uint16_t>(constrain(first, 1.0f, 65535.0f));
        if (lw18.setPeriod(period, period))
        {
            Serial.printf("Period=%u -> %.1f Hz\n", period, 48000000.0f / (period + 1));
        }
        else
        {
            Serial.println("Setting the period failed");
        }
    }
    else if (cmd == "dim" && count >= 2)
    {
        for (LightChannel& channel : channels)
        {
            if (first > 0.0f)
            {
                channel.dimmer().startDimming(true);
            }
            else if (first < 0.0f)
            {
                channel.dimmer().startDimming(false);
            }
            else
            {
                channel.dimmer().stopDimming();
                channel.reportNow();
            }
        }
    }
    else if (cmd == "fx")
    {
        handleEffectCommand(line);
    }
    else if (cmd == "fade")
    {
        if (count >= 2)
        {
            uint16_t transitionTime = static_cast<uint16_t>(constrain(first, 0.0f, 65535.0f));
            Serial.println(zbLight1.writeTransitionTime(transitionTime) ? "Attribute set"
                                                                        : "Setting the attribute failed");
        }
        uint16_t attributeValue = 0;
        bool found = zbLight1.readTransitionTime(attributeValue);
        Serial.printf("OnOffTransitionTime: attribute=%s%u, active=%u (1/10 s)\n", found ? "" : "MISSING ",
                      attributeValue, fadeTimeDs);
    }
    else if (cmd == "zb")
    {
        printZigbeeStatus();
    }
    else if (cmd == "zbreset")
    {
        factoryReset();
    }
    else
    {
        Serial.printf("Unknown: '%s' (type help)\n", line.c_str());
    }
}

void pollSerial()
{
    while (Serial.available() > 0)
    {
        char c = static_cast<char>(Serial.read());
        if (c == '\r' || c == '\n')
        {
            if (inputLine.length() > 0)
            {
                handleCommand(inputLine);
                inputLine = "";
            }
        }
        else if (inputLine.length() < 64)
        {
            inputLine += c;
        }
    }
}

// ---------- Buttons ----------

// Dim direction: off/at the bottom -> up, at the top -> down, otherwise alternating
bool nextDimDirection(Dimmer& dimmer)
{
    bool up = dimUpNext;
    if (!dimmer.isOn() || dimmer.level() <= config::dimmerLevelMin)
    {
        up = true;
    }
    else if (dimmer.level() >= config::zigbeeLevelMax)
    {
        up = false;
    }
    dimUpNext = !up;
    return up;
}

// Local operation stops running effects of the selected channels; returns true if one was running
bool stopEffects(config::ChannelSelect select)
{
    bool stopped = false;
    for (uint8_t i = 0; i < channelCount; ++i)
    {
        Dimmer& dimmer = channels[i].dimmer();
        if (isSelected(i, select) && dimmer.isEffectRunning())
        {
            dimmer.stopEffect();
            stopped = true;
        }
    }
    if (stopped)
    {
        Serial.println("Effect: stopped by button");
    }
    return stopped;
}

void runClickAction(config::LocalAction action, config::ChannelSelect select)
{
    if (action == config::LocalAction::none || action == config::LocalAction::dim || stopEffects(select))
    {
        return;
    }
    // Toggle: if one of the selected channels is on -> all off, otherwise all on
    bool anyOn = false;
    for (uint8_t i = 0; i < channelCount; ++i)
    {
        if (isSelected(i, select) && channels[i].dimmer().isOn())
        {
            anyOn = true;
        }
    }
    for (uint8_t i = 0; i < channelCount; ++i)
    {
        if (!isSelected(i, select))
        {
            continue;
        }
        LightChannel& channel = channels[i];
        if (action == config::LocalAction::toggle)
        {
            channel.dimmer().set(!anyOn, 0);
        }
        else
        {
            channel.dimmer().set(true, config::zigbeeLevelMax);
        }
        channel.reportNow();
    }
}

void onSingle(Button2& button)
{
    uint8_t index = button.getID();
    Serial.printf("Button %u: single\n", index + 1);
    reportButtonEvent(index, config::ButtonEvent::single);
    if (localControl)
    {
        runClickAction(config::localActions[index].single, config::localActions[index].channels);
    }
}

void onDouble(Button2& button)
{
    uint8_t index = button.getID();
    Serial.printf("Button %u: double\n", index + 1);
    reportButtonEvent(index, config::ButtonEvent::doubleClick);
    if (localControl)
    {
        runClickAction(config::localActions[index].doubleClick, config::localActions[index].channels);
    }
}

void onHold(Button2& button)
{
    uint8_t index = button.getID();
    Serial.printf("Button %u: hold\n", index + 1);
    reportButtonEvent(index, config::ButtonEvent::hold);
    const config::ButtonActions& actions = config::localActions[index];
    if (!localControl || actions.hold != config::LocalAction::dim)
    {
        return;
    }
    // Direction from the first selected channel, the same for all selected channels
    bool directionSet = false;
    bool up = true;
    for (uint8_t i = 0; i < channelCount; ++i)
    {
        if (!isSelected(i, actions.channels))
        {
            continue;
        }
        if (!directionSet)
        {
            up = nextDimDirection(channels[i].dimmer());
            directionSet = true;
        }
        channels[i].dimmer().startDimming(up);
    }
}

void onRelease(Button2& button)
{
    uint8_t index = button.getID();
    Serial.printf("Button %u: release\n", index + 1);
    reportButtonEvent(index, config::ButtonEvent::release);
    if (config::localActions[index].hold != config::LocalAction::dim)
    {
        return;
    }
    // Always stop dimming, even if local control/sync mode was changed in the meantime
    for (LightChannel& channel : channels)
    {
        if (channel.dimmer().isDimming())
        {
            channel.dimmer().stopDimming();
            channel.reportNow();
        }
    }
}

void setupButtons()
{
    for (uint8_t i = 0; i < config::buttonCount; ++i)
    {
        Button2& button = buttons[i];
        button.begin(config::pinButton[i], INPUT_PULLUP, true);
        button.setID(i);
        button.setDebounceTime(config::buttonDebounceMs);
        button.setDoubleClickTime(config::buttonDoubleClickMs);
        button.setLongClickTime(config::buttonLongPressMs);
        button.setClickHandler(onSingle);
        button.setDoubleClickHandler(onDouble);
        button.setLongClickDetectedHandler(onHold);
        button.setLongClickHandler(onRelease);
    }

    bootButton.begin(config::pinBootButton, INPUT_PULLUP, true);
    bootButton.setDebounceTime(config::buttonDebounceMs);
    bootButton.setLongClickTime(config::factoryResetHoldMs);
    bootButton.setLongClickDetectedHandler([](Button2&) { factoryReset(); });
}

}  // namespace

void setup()
{
    Serial.begin(config::serialBaud);
    uint32_t start = millis();
    while (!Serial && (millis() - start) < config::serialWaitMs)
    {
        delay(10);
    }
    Serial.setDebugOutput(true);
    // The IDF I2C driver reports every NACK as an error; the LW18 driver logs errors itself
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    pinMode(config::pinStatusLed, OUTPUT);
    setStatusLed(false);

    Serial.println();
    Serial.printf("=== LW18 dimmer %s (%s) ===\n", config::firmwareVersion, config::firmwareDate);

    preferences.begin(config::nvsNamespace, false);
    loadSettings();

    // Free the bus as a precaution (e.g. after a reset during a transfer), then start Wire
    if (!recoverI2cBus(Wire, config::pinSda, config::pinScl, config::i2cFrequency, config::i2cTimeoutMs))
    {
        Serial.println("I2C: SDA/SCL LOW - check wiring/level shifter");
    }

    scanI2c();
    lw18Ready = lw18.begin(config::lw18PeriodReg);
    if (lw18Ready)
    {
        Serial.printf("LW18 ready, period=%u (%.1f Hz)\n", config::lw18PeriodReg,
                      48000000.0f / (config::lw18PeriodReg + 1));
    }
    else
    {
        Serial.println("LW18 not found - retrying during operation");
    }
    lw18LastAttempt = millis();
    lw18LastCheck = millis();

    startChannels();
    updateOutput();

    setupButtons();
    if (config::enableSerialConsole)
    {
        printHelp();
    }
    setupZigbee();
}

void loop()
{
    if (config::enableSerialConsole)
    {
        pollSerial();
    }
    for (Button2& button : buttons)
    {
        button.loop();
    }
    bootButton.loop();
    for (uint8_t i = 0; i < channelCount; ++i)
    {
        applyLight(i);
    }
    applyEffects();
    applySwitches();
    pollZigbeeAttributes();
    for (LightChannel& channel : channels)
    {
        channel.dimmer().loop();
        channel.logIfDone();
        channel.reportIfDue(config::zigbeeTransitionGapMs);
        channel.saveStateIfDue(preferences, config::nvsSaveDelayMs);
    }
    updateOutput();
    superviseLw18();
    updateStatusLed();
    reportStatesPeriodically();
    processReports();
}

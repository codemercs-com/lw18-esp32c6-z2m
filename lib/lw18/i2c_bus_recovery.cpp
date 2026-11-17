// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
#include "i2c_bus_recovery.h"

namespace
{

constexpr uint8_t clockPulses = 9;
constexpr uint32_t halfPeriodUs = 5;  // about 100 kHz

// Emulate open drain: drive LOW actively, HIGH = release (pull-up)
void releaseLine(uint8_t pin)
{
    pinMode(pin, INPUT_PULLUP);
}

void pullLineLow(uint8_t pin)
{
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
}

}  // namespace

bool recoverI2cBus(TwoWire& wire, uint8_t pinSda, uint8_t pinScl, uint32_t frequency, uint16_t timeoutMs)
{
    wire.end();

    releaseLine(pinSda);
    releaseLine(pinScl);
    delayMicroseconds(halfPeriodUs);

    // If a slave holds SDA low, clock until it has finished the started byte
    for (uint8_t pulse = 0; pulse < clockPulses && digitalRead(pinSda) == LOW; ++pulse)
    {
        pullLineLow(pinScl);
        delayMicroseconds(halfPeriodUs);
        releaseLine(pinScl);
        delayMicroseconds(halfPeriodUs);
    }

    // STOP: SDA LOW -> HIGH while SCL is HIGH (change SDA only while SCL is LOW)
    pullLineLow(pinScl);
    delayMicroseconds(halfPeriodUs);
    pullLineLow(pinSda);
    delayMicroseconds(halfPeriodUs);
    releaseLine(pinScl);
    delayMicroseconds(halfPeriodUs);
    releaseLine(pinSda);
    delayMicroseconds(halfPeriodUs);

    bool busFree = (digitalRead(pinSda) == HIGH && digitalRead(pinScl) == HIGH);

    wire.begin(pinSda, pinScl, frequency);
    wire.setTimeOut(timeoutMs);
    return busFree;
}

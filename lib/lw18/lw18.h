// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
// Driver for the Code Mercenaries LED-Warrior18 (I2C, 2 x PWM)
#pragma once

#include <Arduino.h>
#include <Wire.h>

// Content of the signature register (0xF0)
struct Lw18Signature
{
    uint16_t vendor;
    uint16_t product;  // LW18 = 18
    uint16_t version;  // BCD, e.g. 0x1035 = V1.0.3.5
};

class Lw18
{
public:
    static constexpr uint16_t productId = 18;

    Lw18(TwoWire& wire, uint8_t address, uint8_t retries);

    // Checks the signature and sets the PWM period. Returns true if a LW18 was detected.
    // I2C (Wire.begin) must be initialized before.
    bool begin(uint16_t period);

    bool isPresent() const { return m_present; }
    uint8_t address() const { return m_address; }
    uint16_t period() const { return m_period; }

    bool readSignature(Lw18Signature& signature);
    bool setPeriod(uint16_t ch1, uint16_t ch2);
    bool setPwm16(uint16_t ch1, uint16_t ch2);
    bool readPwm16(uint16_t& ch1, uint16_t& ch2);

private:
    enum Register : uint8_t
    {
        regCommand = 0x00,
        regPwm16 = 0x01,
        regPwm8 = 0x02,
        regPeriod = 0x03,
        regSignature = 0xF0,
        regSetAddress = 0xFE,
    };

    bool writeRegister(uint8_t reg, const uint8_t* data, size_t length);
    bool readRegister(uint8_t reg, uint8_t* data, size_t length);
    bool writeTwoWords(uint8_t reg, uint16_t first, uint16_t second);

    TwoWire& m_wire;
    uint8_t m_address;
    uint8_t m_retries;
    uint16_t m_period;
    bool m_present;
};

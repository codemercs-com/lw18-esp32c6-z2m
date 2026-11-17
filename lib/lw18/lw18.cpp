// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
#include "lw18.h"

Lw18::Lw18(TwoWire& wire, uint8_t address, uint8_t retries)
    : m_wire(wire), m_address(address), m_retries(retries), m_period(0xFFFF), m_present(false)
{
}

bool Lw18::begin(uint16_t period)
{
    m_present = false;

    Lw18Signature signature;
    if (!readSignature(signature))
    {
        log_e("LW18 not found at 0x%02X", m_address);
        return false;
    }

    log_i("LW18 signature: vendor=%u product=%u version=%X.%X.%X.%X", signature.vendor, signature.product,
          (signature.version >> 12) & 0xF, (signature.version >> 8) & 0xF, (signature.version >> 4) & 0xF,
          signature.version & 0xF);

    if (signature.product != productId)
    {
        log_e("Unexpected product ID %u (expected %u)", signature.product, productId);
        return false;
    }

    m_present = true;

    // The period is not stored in the LW18 flash -> set it on every start
    if (!setPeriod(period, period))
    {
        log_e("Could not set the period");
        return false;
    }
    return true;
}

bool Lw18::readSignature(Lw18Signature& signature)
{
    uint8_t buffer[6];
    if (!readRegister(regSignature, buffer, sizeof(buffer)))
    {
        return false;
    }
    // The signature is big endian (MSB first)
    signature.vendor = static_cast<uint16_t>((buffer[0] << 8) | buffer[1]);
    signature.product = static_cast<uint16_t>((buffer[2] << 8) | buffer[3]);
    signature.version = static_cast<uint16_t>((buffer[4] << 8) | buffer[5]);
    return true;
}

bool Lw18::setPeriod(uint16_t ch1, uint16_t ch2)
{
    // Byte order like the PWM register (LSB first), verified on the hardware
    if (!writeTwoWords(regPeriod, ch1, ch2))
    {
        return false;
    }
    m_period = ch1;
    return true;
}

bool Lw18::setPwm16(uint16_t ch1, uint16_t ch2)
{
    return writeTwoWords(regPwm16, ch1, ch2);
}

bool Lw18::readPwm16(uint16_t& ch1, uint16_t& ch2)
{
    uint8_t buffer[4];
    if (!readRegister(regPwm16, buffer, sizeof(buffer)))
    {
        return false;
    }
    ch1 = static_cast<uint16_t>(buffer[0] | (buffer[1] << 8));
    ch2 = static_cast<uint16_t>(buffer[2] | (buffer[3] << 8));
    return true;
}

bool Lw18::writeTwoWords(uint8_t reg, uint16_t first, uint16_t second)
{
    const uint8_t data[4] = {
        static_cast<uint8_t>(first & 0xFF),
        static_cast<uint8_t>(first >> 8),
        static_cast<uint8_t>(second & 0xFF),
        static_cast<uint8_t>(second >> 8),
    };
    return writeRegister(reg, data, sizeof(data));
}

bool Lw18::writeRegister(uint8_t reg, const uint8_t* data, size_t length)
{
    uint8_t result = 0;
    for (uint8_t attempt = 0; attempt <= m_retries; ++attempt)
    {
        m_wire.beginTransmission(m_address);
        m_wire.write(reg);
        m_wire.write(data, length);
        result = m_wire.endTransmission();
        if (result == 0)
        {
            return true;
        }
    }
    log_w("I2C write error reg 0x%02X: code %u", reg, result);
    return false;
}

bool Lw18::readRegister(uint8_t reg, uint8_t* data, size_t length)
{
    for (uint8_t attempt = 0; attempt <= m_retries; ++attempt)
    {
        // Set the register pointer, then repeated start and read
        m_wire.beginTransmission(m_address);
        m_wire.write(reg);
        uint8_t result = m_wire.endTransmission(false);
        if (result != 0)
        {
            continue;
        }
        size_t received = m_wire.requestFrom(m_address, length);
        if (received != length)
        {
            continue;
        }
        for (size_t i = 0; i < length; ++i)
        {
            data[i] = static_cast<uint8_t>(m_wire.read());
        }
        return true;
    }
    log_w("I2C read error reg 0x%02X", reg);
    return false;
}

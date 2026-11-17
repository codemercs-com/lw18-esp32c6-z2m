// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
// Free the I2C bus after errors and restart the driver
#pragma once

#include <Arduino.h>
#include <Wire.h>

// Stops the driver, clocks SCL up to 9 times (in case a slave holds SDA low),
// generates a STOP condition and restarts Wire.
// Returns true if SDA and SCL are free (HIGH) afterwards.
bool recoverI2cBus(TwoWire& wire, uint8_t pinSda, uint8_t pinScl, uint32_t frequency, uint16_t timeoutMs);

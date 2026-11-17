// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
// Actively report attributes to the coordinator.
// The library only sets attributes locally; without a reporting configuration by Z2M a change would not arrive.
// Reports are queued and sent with a small gap so the stack is not flooded. Identical entries are merged.
#pragma once

#include <stdint.h>

// Schedule a report; returns false if the queue is full
bool reportAttribute(uint8_t endpoint, uint16_t clusterId, uint16_t attributeId);

// Call from loop(): sends at most one report per interval
void processReports();

// Disable the automatic reporting of the Zigbee stack for an 8 bit attribute (reportable change 255, no
// periodic reports). The stack only reports automatically to bound targets (Z2M binds when configuring).
// Own reports via reportAttribute() keep working. Needed for the brightness: otherwise the stack reports
// every fade step, and a delayed intermediate value overwrites the final value in Z2M.
// Z2M sets up reporting again on "Reconfigure" -> call this regularly.
void disableAutoReporting(uint8_t endpoint, uint16_t clusterId, uint16_t attributeId);

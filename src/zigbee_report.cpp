// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
#include "zigbee_report.h"

#include <Arduino.h>
#include <Zigbee.h>

#include "config.h"

namespace
{

struct PendingReport
{
    uint8_t endpoint;
    uint16_t clusterId;
    uint16_t attributeId;
};

constexpr uint8_t queueSize = 16;
PendingReport queue[queueSize];
uint8_t queueCount = 0;
uint32_t lastSent = 0;

constexpr uint16_t reportingDisabled = 0xFFFF;  // for entries created here (silent anyway without a binding)
constexpr uint8_t unreachableChange = 0xFF;     // reportable change an 8 bit level never reaches

esp_zb_zcl_attr_location_info_t makeLocation(uint8_t endpoint, uint16_t clusterId, uint16_t attributeId)
{
    esp_zb_zcl_attr_location_info_t location = {};
    location.endpoint_id = endpoint;
    location.cluster_id = clusterId;
    location.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    location.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    location.attr_id = attributeId;
    return location;
}

// Without a reporting entry esp_zb_zcl_report_attr_cmd_req fails with an assertion (crash).
// Z2M creates the entries only when configuring -> create them here for new endpoints.
// Call only while holding the Zigbee lock.
bool ensureReportingInfo(const PendingReport& report)
{
    esp_zb_zcl_attr_location_info_t location = makeLocation(report.endpoint, report.clusterId, report.attributeId);
    if (esp_zb_zcl_find_reporting_info(location) != nullptr)
    {
        return true;
    }

    esp_zb_zcl_reporting_info_t info = {};
    info.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    info.ep = report.endpoint;
    info.cluster_id = report.clusterId;
    info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    info.attr_id = report.attributeId;
    info.u.send_info.min_interval = 0;
    info.u.send_info.max_interval = reportingDisabled;  // own reports only
    info.u.send_info.def_min_interval = 0;
    info.u.send_info.def_max_interval = reportingDisabled;
    info.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    info.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    esp_err_t result = esp_zb_zcl_update_reporting_info(&info);
    if (result != ESP_OK)
    {
        Serial.printf("Reporting entry missing and could not be created (EP %u, cluster 0x%04X): %s\n", report.endpoint,
                      report.clusterId, esp_err_to_name(result));
        return false;
    }
    return true;
}

bool sendReport(const PendingReport& report)
{
    esp_zb_zcl_report_attr_cmd_t reportCmd = {};
    reportCmd.zcl_basic_cmd.dst_addr_u.addr_short = config::zigbeeCoordinatorAddress;
    reportCmd.zcl_basic_cmd.dst_endpoint = config::zigbeeCoordinatorEndpoint;
    reportCmd.zcl_basic_cmd.src_endpoint = report.endpoint;
    reportCmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    reportCmd.clusterID = report.clusterId;
    reportCmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    reportCmd.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    reportCmd.attributeID = report.attributeId;

    esp_zb_lock_acquire(portMAX_DELAY);
    if (!ensureReportingInfo(report))
    {
        esp_zb_lock_release();
        return false;
    }
    esp_err_t result = esp_zb_zcl_report_attr_cmd_req(&reportCmd);
    esp_zb_lock_release();
    if (result != ESP_OK)
    {
        Serial.printf("Zigbee report failed (EP %u, cluster 0x%04X): %s\n", report.endpoint, report.clusterId,
                      esp_err_to_name(result));
        return false;
    }
    return true;
}

}  // namespace

bool reportAttribute(uint8_t endpoint, uint16_t clusterId, uint16_t attributeId)
{
    if (!Zigbee.connected())
    {
        return false;
    }
    // Already scheduled -> the report reads the current value when it is sent anyway
    for (uint8_t i = 0; i < queueCount; ++i)
    {
        const PendingReport& pending = queue[i];
        if (pending.endpoint == endpoint && pending.clusterId == clusterId && pending.attributeId == attributeId)
        {
            return true;
        }
    }
    if (queueCount >= queueSize)
    {
        Serial.printf("Zigbee report dropped (queue full, EP %u)\n", endpoint);
        return false;
    }
    queue[queueCount++] = {endpoint, clusterId, attributeId};
    return true;
}

void disableAutoReporting(uint8_t endpoint, uint16_t clusterId, uint16_t attributeId)
{
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_reporting_info_t* existing =
        esp_zb_zcl_find_reporting_info(makeLocation(endpoint, clusterId, attributeId));
    bool changed = false;
    esp_err_t result = ESP_OK;
    // Reportable change 255: an 8 bit level never changes that much -> no report on change.
    // Max. interval 0: no periodic reports. (0xFFFF does not work in the stack as defined by the ZCL.)
    bool suppressed = existing != nullptr && existing->u.send_info.delta.u8 == unreachableChange &&
                      existing->u.send_info.max_interval == 0;
    if (existing != nullptr && !suppressed)
    {
        esp_zb_zcl_reporting_info_t info = *existing;  // keep the target (coordinator) etc.
        info.u.send_info.delta.u32 = 0;
        info.u.send_info.delta.u8 = unreachableChange;
        info.u.send_info.max_interval = 0;
        info.u.send_info.def_max_interval = 0;
        result = esp_zb_zcl_update_reporting_info(&info);
        changed = true;
    }
    esp_zb_lock_release();
    if (changed)
    {
        Serial.printf("Auto reporting EP %u cluster 0x%04X attr 0x%04X %s\n", endpoint, clusterId, attributeId,
                      result == ESP_OK ? "disabled" : "could not be disabled");
    }
}

void processReports()
{
    uint32_t now = millis();
    if (queueCount == 0 || now - lastSent < config::zigbeeReportIntervalMs)
    {
        return;
    }
    lastSent = now;
    PendingReport report = queue[0];
    for (uint8_t i = 1; i < queueCount; ++i)
    {
        queue[i - 1] = queue[i];
    }
    --queueCount;
    if (Zigbee.connected())
    {
        sendReport(report);
    }
}

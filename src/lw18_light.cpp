// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
#include "lw18_light.h"

#include <string.h>

// The ZBOSS headers declare their C functions without extern "C"
extern "C"
{
#include "zboss_api.h"
}

namespace
{
constexpr uint8_t identifyTriggerEffectCommandId = 0x40;  // ZCL Identify: Trigger Effect
}

Lw18Light* Lw18Light::m_instances[Lw18Light::maxInstances] = {};
uint8_t Lw18Light::m_instanceCount = 0;

Lw18Light::Lw18Light(uint8_t endpoint) : ZigbeeDimmableLight(endpoint)
{
}

void Lw18Light::onTriggerEffect(EffectCallback callback)
{
    m_effectCallback = callback;
    if (findInstance(_endpoint) == nullptr && m_instanceCount < maxInstances)
    {
        m_instances[m_instanceCount++] = this;
    }
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_raw_command_handler_register(rawCommandHandler);
    esp_zb_lock_release();
}

Lw18Light* Lw18Light::findInstance(uint8_t endpoint)
{
    for (uint8_t i = 0; i < m_instanceCount; ++i)
    {
        if (m_instances[i]->_endpoint == endpoint)
        {
            return m_instances[i];
        }
    }
    return nullptr;
}

// Called by the Zigbee stack for every incoming ZCL command.
// true = handled here (buffer released), false = the stack continues as usual.
bool Lw18Light::rawCommandHandler(uint8_t bufferId)
{
    zb_zcl_parsed_hdr_t* header = ZB_BUF_GET_PARAM(bufferId, zb_zcl_parsed_hdr_t);
    bool isTriggerEffect = header->cluster_id == ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY && !header->is_common_command &&
                           header->cmd_direction == ZB_ZCL_FRAME_DIRECTION_TO_SRV &&
                           header->cmd_id == identifyTriggerEffectCommandId;
    if (!isTriggerEffect)
    {
        return false;
    }
    Lw18Light* instance = findInstance(ZB_ZCL_PARSED_HDR_SHORT_DATA(header).dst_endpoint);
    if (instance == nullptr || instance->m_effectCallback == nullptr)
    {
        return false;
    }

    zb_zcl_status_t status = ZB_ZCL_STATUS_MALFORMED_CMD;
    if (zb_buf_len(bufferId) >= 2)
    {
        const uint8_t* payload = static_cast<const uint8_t*>(zb_buf_begin(bufferId));
        instance->m_effectCallback(payload[0], payload[1]);
        status = ZB_ZCL_STATUS_SUCCESS;
    }
    zb_zcl_send_default_handler(bufferId, header, status);
    return true;
}

bool Lw18Light::setSoftwareVersion(const char* version, const char* date)
{
    bool versionOk = setBasicString(ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, version, m_versionZcl);
    bool dateOk = setBasicString(ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, date, m_dateZcl);
    return versionOk && dateOk;
}

bool Lw18Light::setBasicString(uint16_t attributeId, const char* text, char* buffer)
{
    size_t length = strlen(text);
    if (length > zclStringMax)
    {
        return false;
    }
    buffer[0] = static_cast<char>(length);
    memcpy(buffer + 1, text, length);
    buffer[length + 1] = '\0';
    esp_zb_attribute_list_t* basicCluster =
        esp_zb_cluster_list_get_cluster(_cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (basicCluster == nullptr)
    {
        return false;
    }
    // The attribute may already exist (default value) -> replace it then
    if (esp_zb_basic_cluster_add_attr(basicCluster, attributeId, buffer) == ESP_OK)
    {
        return true;
    }
    return esp_zb_cluster_update_attr(basicCluster, attributeId, buffer) == ESP_OK;
}

bool Lw18Light::addTransitionTimeAttribute(uint16_t transitionTime)
{
    m_transitionTime = transitionTime;
    esp_zb_attribute_list_t* levelCluster = esp_zb_cluster_list_get_cluster(
        _cluster_list, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (levelCluster == nullptr)
    {
        return false;
    }
    return esp_zb_level_cluster_add_attr(levelCluster, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_OFF_TRANSITION_TIME_ID,
                                         &m_transitionTime) == ESP_OK;
}

bool Lw18Light::addStartUpOnOffAttribute(StartUpOnOff startUpOnOff)
{
    m_startUpOnOff = static_cast<uint8_t>(startUpOnOff);
    esp_zb_attribute_list_t* onOffCluster =
        esp_zb_cluster_list_get_cluster(_cluster_list, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (onOffCluster == nullptr)
    {
        return false;
    }
    return esp_zb_on_off_cluster_add_attr(onOffCluster, ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF, &m_startUpOnOff) ==
           ESP_OK;
}

bool Lw18Light::readTransitionTime(uint16_t& transitionTime)
{
    return readAttribute(ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_OFF_TRANSITION_TIME_ID,
                         &transitionTime, sizeof(transitionTime));
}

bool Lw18Light::writeTransitionTime(uint16_t transitionTime)
{
    return writeAttribute(ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_OFF_TRANSITION_TIME_ID,
                          &transitionTime);
}

bool Lw18Light::readStartUpOnOff(StartUpOnOff& startUpOnOff)
{
    uint8_t value = 0;
    if (!readAttribute(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF, &value, sizeof(value)))
    {
        return false;
    }
    startUpOnOff = static_cast<StartUpOnOff>(value);
    return true;
}

bool Lw18Light::writeStartUpOnOff(StartUpOnOff startUpOnOff)
{
    uint8_t value = static_cast<uint8_t>(startUpOnOff);
    return writeAttribute(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF, &value);
}

bool Lw18Light::isValidStartUpOnOff(uint8_t value)
{
    return value == static_cast<uint8_t>(StartUpOnOff::off) || value == static_cast<uint8_t>(StartUpOnOff::on) ||
           value == static_cast<uint8_t>(StartUpOnOff::toggle) || value == static_cast<uint8_t>(StartUpOnOff::previous);
}

bool Lw18Light::readAttribute(uint16_t clusterId, uint16_t attributeId, void* value, size_t size)
{
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_attr_t* attribute =
        esp_zb_zcl_get_attribute(_endpoint, clusterId, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attributeId);
    bool found = (attribute != nullptr && attribute->data_p != nullptr);
    if (found)
    {
        memcpy(value, attribute->data_p, size);
    }
    esp_zb_lock_release();
    return found;
}

bool Lw18Light::writeAttribute(uint16_t clusterId, uint16_t attributeId, void* value)
{
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_status_t status =
        esp_zb_zcl_set_attribute_val(_endpoint, clusterId, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attributeId, value, false);
    esp_zb_lock_release();
    return status == ESP_ZB_ZCL_STATUS_SUCCESS;
}

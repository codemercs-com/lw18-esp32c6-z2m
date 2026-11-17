// Zigbee2MQTT External Converter: LW18-Dimmer-4T (XIAO ESP32-C6 + LED-Warrior18)
// Location: <z2m>/data/external_converters/lw18_dimmer_4t.mjs, then restart Z2M
// SPDX-FileCopyrightText: 2026 Code Mercenaries Hard- und Software GmbH
// SPDX-License-Identifier: MIT
import * as exposes from "zigbee-herdsman-converters/lib/exposes";
import * as m from "zigbee-herdsman-converters/lib/modernExtend";

const e = exposes.presets;
const ea = exposes.access;

// Endpoint names -> Zigbee endpoint of the firmware
const lightEndpoints = {light: 10, light_2: 16};

// Light effects -> Identify "Trigger Effect" ID (must match config::lightEffects in the firmware)
const lightEffects = {
    blink: 0x00,
    breathe: 0x01,
    okay: 0x02,
    channel_change: 0x0b,
    pulse_fast: 0x10,
    pulse_slow: 0x11,
    alarm: 0x12,
    finish_effect: 0xfe,
    stop_effect: 0xff,
};

// Own effect list per channel instead of the default effects of m.light
const effects = {
    isModernExtend: true,
    exposes: Object.keys(lightEndpoints).map((name) =>
        e.enum("effect", ea.SET, Object.keys(lightEffects)).withEndpoint(name).withDescription("Trigger a light effect"),
    ),
    toZigbee: [
        {
            key: ["effect"],
            convertSet: async (entity, key, value, meta) => {
                const effectid = lightEffects[value];
                if (effectid === undefined) {
                    throw new Error(`Unknown effect '${value}'`);
                }
                const endpoint = meta.device.getEndpoint(lightEndpoints[meta.endpoint_name] ?? lightEndpoints.light);
                await endpoint.command("genIdentify", "triggerEffect", {effectid, effectvariant: 0}, {disableDefaultResponse: true});
            },
        },
    ],
};

// Power on behavior = StartUpOnOff (genOnOff 0x4003), applies to both channels, attribute on endpoint 10
const powerOnLookup = {off: 0x00, on: 0x01, toggle: 0x02, previous: 0xff};
const powerOnBehavior = {
    isModernExtend: true,
    exposes: [
        e
            .enum("power_on_behavior", ea.ALL, Object.keys(powerOnLookup))
            .withEndpoint("light")
            .withDescription("State after a power failure (both channels)")
            .withCategory("config"),
    ],
    fromZigbee: [
        {
            cluster: "genOnOff",
            type: ["attributeReport", "readResponse"],
            convert: (model, msg, publish, options, meta) => {
                if (msg.endpoint.ID !== lightEndpoints.light || msg.data.startUpOnOff === undefined) {
                    return;
                }
                const value = Object.keys(powerOnLookup).find((key) => powerOnLookup[key] === msg.data.startUpOnOff);
                return value ? {power_on_behavior_light: value} : undefined;
            },
        },
    ],
    toZigbee: [
        {
            key: ["power_on_behavior"],
            convertSet: async (entity, key, value, meta) => {
                const startUpOnOff = powerOnLookup[value];
                if (startUpOnOff === undefined) {
                    throw new Error(`Unknown value '${value}'`);
                }
                await meta.device.getEndpoint(lightEndpoints.light).write("genOnOff", {startUpOnOff});
                return {state: {power_on_behavior: value}};
            },
            convertGet: async (entity, key, meta) => {
                await meta.device.getEndpoint(lightEndpoints.light).read("genOnOff", ["startUpOnOff"]);
            },
        },
    ],
};

// Endpoint of the firmware -> button name
const buttonEndpoints = {11: "button_1", 12: "button_2", 13: "button_3", 14: "button_4"};
// Multistate input PresentValue -> event (0 = idle, ignored)
const buttonEvents = {1: "single", 2: "double", 3: "hold", 4: "release"};

const actions = Object.values(buttonEndpoints).flatMap((button) =>
    Object.values(buttonEvents).map((event) => `${button}_${event}`),
);

// Button events -> action "button_<n>_<event>"
const buttonActions = {
    isModernExtend: true,
    exposes: [e.action(actions)],
    fromZigbee: [
        {
            cluster: "genMultistateInput",
            type: ["attributeReport", "readResponse"],
            convert: (model, msg, publish, options, meta) => {
                const button = buttonEndpoints[msg.endpoint.ID];
                const event = buttonEvents[msg.data.presentValue];
                if (!button || !event) {
                    return;
                }
                return {action: `${button}_${event}`};
            },
        },
    ],
};

export default {
    zigbeeModel: ["LW18-Dimmer-4T"],
    model: "LW18-Dimmer-4T",
    vendor: "DIY",
    description: "LED dimmer (LED-Warrior18) with 2 channels and 4 buttons",
    extend: [
        m.deviceEndpoints({
            endpoints: {light: 10, button_1: 11, button_2: 12, button_3: 13, button_4: 14, local: 15, light_2: 16, sync: 17},
        }),
        // Channel 1 (light) and channel 2 (light_2). In sync mode (sync = on) each slider controls both channels.
        // effect/powerOnBehavior: false -> own variants, see "effects" and "powerOnBehavior"
        m.light({endpointNames: ["light", "light_2"], powerOnBehavior: false, effect: false}),
        effects,
        powerOnBehavior,
        // Fade time = OnOffTransitionTime (1/10 s), applies to both channels. Own numeric expose instead of levelConfig:
        // in the frontend the composite expose on a named endpoint always sends the old value.
        m.numeric({
            name: "fade_time",
            label: "Fade time",
            description: "Fade time for on/off and buttons, both channels (0 = switch instantly)",
            cluster: "genLevelCtrl",
            attribute: "onOffTransitionTime",
            endpointNames: ["light"],
            unit: "s",
            valueMin: 0,
            valueMax: 60,
            valueStep: 0.1,
            scale: 10,
            access: "ALL",
            entityCategory: "config",
        }),
        // local = local control with the buttons, sync = channels in sync
        m.onOff({endpointNames: ["local", "sync"], powerOnBehavior: false}),
        buttonActions,
    ],
};

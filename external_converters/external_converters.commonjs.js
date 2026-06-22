const { battery, illuminance } = require('zigbee-herdsman-converters/lib/modernExtend');
const fromZigbee_1 = require("zigbee-herdsman-converters/converters/fromZigbee");
const legacy = require("zigbee-herdsman-converters/lib/legacy");
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const utils = require('zigbee-herdsman-converters/lib/utils');
const e = exposes.presets;
const ea = exposes.access;
const tuya = require("zigbee-herdsman-converters/lib/tuya");

const definition = [{
    fingerprint: tuya.fingerprint('TS0601', [
        '_TZE284_o3x45p96' /* model: 'TRV06', vendor: 'AVATTO' */,
    ]),
    model: 'TS0601_thermostat_3',
    vendor: 'Tuya',
    description: 'Thermostatic radiator valve',
    fromZigbee: [tuya.fz.datapoints],
    toZigbee: [tuya.tz.datapoints],
    whiteLabel: [
        tuya.whitelabel('AVATTO', 'TRV06_1', 'Thermostatic radiator valve', ['_TZE284_o3x45p96']),
    ],
    onEvent: tuya.onEventSetTime,
    configure: tuya.configureMagicPacket,
    exposes: [
        e.child_lock(),
        e.battery_low(),
        e
            .climate()
            .withSetpoint('current_heating_setpoint', 5, 35, 1, ea.STATE_SET)
            .withLocalTemperature(ea.STATE)
            .withSystemMode(['auto', 'heat', 'off'], ea.STATE_SET)
            .withRunningState(['idle', 'heat'], ea.STATE)
            .withLocalTemperatureCalibration(-9, 9, 1, ea.STATE_SET),
        ...tuya.exposes.scheduleAllDays(ea.STATE_SET, 'HH:MM/C HH:MM/C HH:MM/C HH:MM/C HH:MM/C HH:MM/C'),
        e
            .binary('scale_protection', ea.STATE_SET, 'ON', 'OFF')
            .withDescription('If the heat sink is not fully opened within two weeks or is not used for a long time, the valve will be blocked due to silting up and the heat sink will not be able to be used.'),
        e
            .binary('frost_protection', ea.STATE_SET, 'ON', 'OFF')
            .withDescription('When the room temperature is lower than 5 C, the valve opens; when the temperature rises to 8 C, the valve closes.'),
        e.numeric('error', ea.STATE).withDescription('If NTC is damaged, "Er" will be on the TRV display.'),
    ],
    meta: {
        tuyaDatapoints: [
            [2, 'system_mode', tuya.valueConverterBasic.lookup({ auto: tuya.enum(0), heat: tuya.enum(1), off: tuya.enum(2) })],
            [3, 'running_state', tuya.valueConverterBasic.lookup({ heat: tuya.enum(0), idle: tuya.enum(1) })],
            [4, 'current_heating_setpoint', tuya.valueConverter.divideBy10],
            [5, 'local_temperature', tuya.valueConverter.divideBy10],
            [7, 'child_lock', tuya.valueConverter.lockUnlock],
            [28, 'schedule_monday', tuya.valueConverter.thermostatScheduleDayMultiDPWithDayNumber(1)],
            [29, 'schedule_tuesday', tuya.valueConverter.thermostatScheduleDayMultiDPWithDayNumber(2)],
            [30, 'schedule_wednesday', tuya.valueConverter.thermostatScheduleDayMultiDPWithDayNumber(3)],
            [31, 'schedule_thursday', tuya.valueConverter.thermostatScheduleDayMultiDPWithDayNumber(4)],
            [32, 'schedule_friday', tuya.valueConverter.thermostatScheduleDayMultiDPWithDayNumber(5)],
            [33, 'schedule_saturday', tuya.valueConverter.thermostatScheduleDayMultiDPWithDayNumber(6)],
            [34, 'schedule_sunday', tuya.valueConverter.thermostatScheduleDayMultiDPWithDayNumber(7)],
            [35, null, tuya.valueConverter.errorOrBatteryLow],
            [36, 'frost_protection', tuya.valueConverter.onOff],
            [39, 'scale_protection', tuya.valueConverter.onOff],
            [47, 'local_temperature_calibration', tuya.valueConverter.localTempCalibration2],
        ],
    },
},
{
    zigbeeModel: ['TS0222'],
    model: 'TS0222',
    vendor: '_TZ3000_hy6ncvmw',
    description: 'Light intensity sensor',
    extend: [battery(), illuminance()],
    meta: {},
},
{
    fingerprint: [{modelID: 'WaterMeter', manufacturerName: 'ZigbeeHive'}],
    zigbeeModel: ['WaterMeter'],
    model: 'ZigbeeHive-WaterMeter',
    vendor: 'ZigbeeHive',
    description: 'ESP32-C6 Zigbee water meter',
    fromZigbee: [{
        cluster: 'seMetering',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg) => {
            const u48 = (v) => {
                if (typeof v === 'number') return v;
                if (typeof v === 'bigint') return Number(v);
                if (v && typeof v === 'object') return ((v.high ?? v[1] ?? 0) * 0x100000000) + (v.low ?? v[0] ?? 0);
                return v;
            };
            const r = {};
            if (msg.data.currentSummDelivered !== undefined) r.pulse_count = u48(msg.data.currentSummDelivered);
            if (msg.data.multiplier !== undefined) r.multiplier = msg.data.multiplier;
            if (msg.data.divisor !== undefined) r.divisor = msg.data.divisor;
            if (msg.data[0xFC00] !== undefined) r.scaled_summation = u48(msg.data[0xFC00]);
            if (msg.data[0xFC01] !== undefined) r.scale_multiplier = msg.data[0xFC01];
            if (msg.data[0xFC02] !== undefined) r.scale_divisor = msg.data[0xFC02];
            return r;
        },
    }],
    toZigbee: [{
        key: ['scale_multiplier', 'scale_divisor'],
        convertSet: async (entity, key, value, meta) => {
            const n = Number(value);
            if (!Number.isInteger(n) || n <= 0) throw new Error(`${key} must be a positive integer`);
            const attr = key === 'scale_multiplier' ? 0xFC01 : 0xFC02;
            await entity.write('seMetering', {[attr]: {value: n, type: 0x23}}, utils.getOptions(meta.mapped, entity));
            await entity.read('seMetering', ['multiplier', 'divisor', 0xFC00, 0xFC01, 0xFC02]);
            return {state: {[key]: n}};
        },
        convertGet: async (entity, key) => {
            await entity.read('seMetering', [key === 'scale_multiplier' ? 0xFC01 : 0xFC02]);
        },
    },
    {
        key: ['pulse_count', 'scaled_summation', 'multiplier', 'divisor'],
        convertGet: async (entity, key) => {
            const attrs = {
                pulse_count: 'currentSummDelivered',
                scaled_summation: 0xFC00,
                multiplier: 'multiplier',
                divisor: 'divisor',
            };
            await entity.read('seMetering', [attrs[key]]);
        },
    }],
    exposes: [
        e.numeric('pulse_count', ea.STATE_GET),
        e.numeric('scaled_summation', ea.STATE_GET),
        e.numeric('scale_multiplier', ea.STATE_SET).withValueMin(1),
        e.numeric('scale_divisor', ea.STATE_SET).withValueMin(1),
        e.numeric('multiplier', ea.STATE_GET),
        e.numeric('divisor', ea.STATE_GET),
        e.battery(),
        e.battery_voltage(),
    ],
    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(1);
        await reporting.bind(endpoint, coordinatorEndpoint, ['seMetering', 'genPowerCfg']);
        await endpoint.read('seMetering', ['currentSummDelivered', 'multiplier', 'divisor', 0xFC00, 0xFC01, 0xFC02]);
        await endpoint.read('genPowerCfg', ['batteryVoltage', 'batteryPercentageRemaining']);
    },
}
];

module.exports = definition;

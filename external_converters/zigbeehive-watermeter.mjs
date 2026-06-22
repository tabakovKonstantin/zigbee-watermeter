import * as exposes from 'zigbee-herdsman-converters/lib/exposes';
import * as reporting from 'zigbee-herdsman-converters/lib/reporting';
import * as utils from 'zigbee-herdsman-converters/lib/utils';

const e = exposes.presets;
const ea = exposes.access;

const ATTR_SCALED_SUMMATION = 0xFC00;
const ATTR_SCALE_MULTIPLIER = 0xFC01;
const ATTR_SCALE_DIVISOR = 0xFC02;
const TYPE_UINT32 = 0x23;

function uint48ToNumber(value) {
    if (typeof value === 'number') {
        return value;
    }

    if (typeof value === 'bigint') {
        return Number(value);
    }

    if (value && typeof value === 'object') {
        const low = value.low ?? value[0] ?? 0;
        const high = value.high ?? value[1] ?? 0;
        return (high * 0x100000000) + low;
    }

    return value;
}

const fzWaterMeter = {
    cluster: 'seMetering',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        const result = {};

        if (msg.data.currentSummDelivered !== undefined) {
            result.pulse_count = uint48ToNumber(msg.data.currentSummDelivered);
        }

        if (msg.data.multiplier !== undefined) {
            result.multiplier = msg.data.multiplier;
        }

        if (msg.data.divisor !== undefined) {
            result.divisor = msg.data.divisor;
        }

        if (msg.data[ATTR_SCALED_SUMMATION] !== undefined) {
            result.scaled_summation = uint48ToNumber(msg.data[ATTR_SCALED_SUMMATION]);
            result.water_consumed = result.scaled_summation;
        }

        if (msg.data[ATTR_SCALE_MULTIPLIER] !== undefined) {
            result.scale_multiplier = msg.data[ATTR_SCALE_MULTIPLIER];
        }

        if (msg.data[ATTR_SCALE_DIVISOR] !== undefined) {
            result.scale_divisor = msg.data[ATTR_SCALE_DIVISOR];
        }

        return result;
    },
};

const tzScale = {
    key: ['scale_multiplier', 'scale_divisor'],
    convertSet: async (entity, key, value, meta) => {
        const numericValue = Number(value);
        if (!Number.isInteger(numericValue) || numericValue <= 0) {
            throw new Error(`${key} must be a positive integer`);
        }

        const attr = key === 'scale_multiplier' ? ATTR_SCALE_MULTIPLIER : ATTR_SCALE_DIVISOR;
        await entity.write('seMetering', {
            [attr]: {value: numericValue, type: TYPE_UINT32},
        }, utils.getOptions(meta.mapped, entity));

        await entity.read('seMetering', [
            'multiplier',
            'divisor',
            ATTR_SCALED_SUMMATION,
            ATTR_SCALE_MULTIPLIER,
            ATTR_SCALE_DIVISOR,
        ]);

        return {state: {[key]: numericValue}};
    },
    convertGet: async (entity, key, meta) => {
        const attr = key === 'scale_multiplier' ? ATTR_SCALE_MULTIPLIER : ATTR_SCALE_DIVISOR;
        await entity.read('seMetering', [attr]);
    },
};

const tzReadWaterMeter = {
    key: ['pulse_count', 'scaled_summation', 'water_consumed', 'multiplier', 'divisor'],
    convertGet: async (entity, key, meta) => {
        const attrByKey = {
            pulse_count: 'currentSummDelivered',
            scaled_summation: ATTR_SCALED_SUMMATION,
            water_consumed: ATTR_SCALED_SUMMATION,
            multiplier: 'multiplier',
            divisor: 'divisor',
        };
        await entity.read('seMetering', [attrByKey[key]]);
    },
};

export default {
    fingerprint: [
        {modelID: 'WaterMeter', manufacturerName: 'ZigbeeHive'},
    ],
    zigbeeModel: ['WaterMeter'],
    model: 'ZigbeeHive-WaterMeter',
    vendor: 'ZigbeeHive',
    description: 'ESP32-C6 Zigbee water meter',
    fromZigbee: [fzWaterMeter],
    toZigbee: [tzScale, tzReadWaterMeter],
    exposes: [
        e.numeric('pulse_count', ea.STATE_GET)
            .withDescription('Raw hall sensor pulse counter stored in device NVS'),
        e.numeric('scaled_summation', ea.STATE_GET)
            .withUnit('L')
            .withDescription('pulse_count * scale_multiplier / scale_divisor'),
        e.numeric('water_consumed', ea.STATE_GET)
            .withUnit('L')
            .withDescription('Water consumed for Home Assistant Energy'),
        e.numeric('scale_multiplier', ea.STATE_SET)
            .withValueMin(1)
            .withDescription('Writable scale multiplier stored in device NVS'),
        e.numeric('scale_divisor', ea.STATE_SET)
            .withValueMin(1)
            .withDescription('Writable scale divisor stored in device NVS'),
        e.numeric('multiplier', ea.STATE_GET)
            .withDescription('Standard metering multiplier mirror'),
        e.numeric('divisor', ea.STATE_GET)
            .withDescription('Standard metering divisor mirror'),
        e.battery(),
        e.battery_voltage(),
    ],
    configure: async (device, coordinatorEndpoint, logger) => {
        const endpoint = device.getEndpoint(1);
        await reporting.bind(endpoint, coordinatorEndpoint, ['seMetering', 'genPowerCfg']);
        await endpoint.read('seMetering', [
            'currentSummDelivered',
            'multiplier',
            'divisor',
            ATTR_SCALED_SUMMATION,
            ATTR_SCALE_MULTIPLIER,
            ATTR_SCALE_DIVISOR,
        ]);
        await endpoint.read('genPowerCfg', ['batteryVoltage', 'batteryPercentageRemaining']);
    },
};

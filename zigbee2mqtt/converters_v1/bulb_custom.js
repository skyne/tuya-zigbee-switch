const {
    numeric,
    enumLookup,
    deviceEndpoints,
    light,
    text,
    binary,
    deviceAddCustomCluster,
} = require("zigbee-herdsman-converters/lib/modernExtend");
const {assertString} = require("zigbee-herdsman-converters/lib/utils");
const reporting = require("zigbee-herdsman-converters/lib/reporting");
const constants = require("zigbee-herdsman-converters/lib/constants");
const Zcl = require('zigbee-herdsman').Zcl;

/********************************************************************
  This file (`bulb_custom.js`) is generated. 
  
  You can edit it for testing, but for PRs please use:
  - `device_db.yaml`                - add or edit devices
  - `bulb_custom.md.jinja`          - update the template
  - `make_z2m_custom_converters.py` - update generation script

  Generate with: `make converters`
********************************************************************/

const romasku = {
    networkIndicator: (name, endpointName) =>
        binary({
            name,
            endpointName,
            valueOn: ["ON", 1],
            valueOff: ["OFF", 0],
            cluster: "genBasic",
            attribute: {ID: 0xff01, type: 0x10},  // Boolean
            description: "State of the network indicator LED",
            access: "ALL",
            entityCategory: "config",
        }),
    deviceConfig: (name, endpointName) =>
        text({
            name,
            endpointName,
            access: "ALL",
            cluster: "genBasic",
            attribute:  { ID: 0xff00, type: 0x44 }, // long str
            description: "Current configuration of the device",
            zigbeeCommandOptions: {timeout: 30_000},
            validate: (value) => {
                assertString(value);
                
                const validatePin = (pin) => {
                    const validPins = [
                        "A0", "A1", "A2", "A3", "A4", "A5", "A6","A7",
                        "B0", "B1", "B2", "B3", "B4", "B5", "B6","B7",
                        "C0", "C1", "C2", "C3", "C4", "C5", "C6","C7",
                        "D0", "D1", "D2", "D3", "D4", "D5", "D6","D7",
                        "E0", "E1", "E2", "E3",
                    ];
                    if (!validPins.includes(pin)) throw new Error(`Pin ${pin} is invalid`);
                }

                if (value.length > 256) throw new Error('Length of config is greater than 256');
                if (!value.endsWith(';')) throw new Error('Should end with ;');
                const parts = value.slice(0, -1).split(';');  // Drop last ;
                if (parts.length < 2) throw new Error("Model and/or manufacturer missing");
                for (const part of parts.slice(2)) {
                    if (part == 'SLP') {
                        continue;   
                    } if (part[0] == 'P' && part.length == 4) {
                        // PWM pin configuration (e.g., PRA0, PGB1, etc.)
                        validatePin(part.slice(2,4));
                    } else if (part[0] == 'U') {
                        // UART or other U-prefixed config
                        continue;
                    } else if(part[0] == 'M') {
                        ; // Model info
                    } else if(part[0] == 'i') {
                        ; // Additional info
                    } else {
                        throw new Error(`Invalid entry ${part}. Should start with one of P (PWM), U, M, or i`);
                    }
                }
            },
            entityCategory: "config",
        }),
};

const definitions = [
    {
        zigbeeModel: [
            "CK-BL702-AL-01_1",
        ],
        model: "TS0505B",
        vendor: "Tuya-custom",
        description: "Custom bulb (https://github.com/romasku/tuya-zigbee-switch)",
        extend: [
            romasku.deviceConfig("device_config", undefined),
            light({
                colorTemp: {range: [153, 500]},
                color: {modes: ["xy", "hs"]},
            }),
        ],
        configure: async (device, coordinatorEndpoint, logger) => {
            const endpoint = device.getEndpoint(1);
            await reporting.bind(endpoint, coordinatorEndpoint, ["genOnOff", "genLevelCtrl", "lightingColorCtrl"]);
            await reporting.onOff(endpoint);
            await reporting.brightness(endpoint);
            await reporting.colorTemperature(endpoint);
            await endpoint.configureReporting('lightingColorCtrl', [{
                attribute: 'currentX',
                minimumReportInterval: 0,
                maximumReportInterval: constants.repInterval.HOUR,
                reportableChange: 1,
            }, {
                attribute: 'currentY',
                minimumReportInterval: 0,
                maximumReportInterval: constants.repInterval.HOUR,
                reportableChange: 1,
            }]);
        },
        ota: true,
    },
];

module.exports = definitions;

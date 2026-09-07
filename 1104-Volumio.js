// sudo apt install -y mc make g++ gpiod
// npm install i2c-bus
// npm install socket.io-client2@npm:socket.io-client@2.4.0 --legacy-peer-deps
// systemctl start snapserver && systemctl enable snapserver
'use strict';

const { spawn } = require('child_process');
const i2c = require('i2c-bus');

const GPIO_CHIP = '/dev/gpiochip0';
const GPIO_LINE = '4';

const I2C_BUS = 1;
const MSPM0_ADDRESS = 0x77;
const DATA_LENGTH = 8;

const VOLUMIO_HOST = 'localhost';

let gpio;
let i2cBus;
let lastRepeat;

let next;
let delay;
const INITIAL_DELAY = 500;
const MINIMUM_DELAY = 100;
const DECAY = .85;
const POWER_DELAY = 800;
const NUMBER_DELAY = 800;
const TRACK_DELAY = 1000;

let key;
let press_time;
let release_time;
let press_count;
let track_number = null;
let trackTimer = null;

const table = [
    { "scancode": 0x00000000, "keycode": 255,     "keycommand": "RELEASE" },                 // Not used
    { "scancode": 0x00000001, "keycode": 28,      "keycommand": "toggle" },                  // KEY_ENTER Rotary Button
    { "scancode": 0x0100,     "keycode": 0x16d,   "keycommand": "playplaylist&name=IR_3" },  // KEY_EPG
    { "scancode": 0x0101,     "keycode": 0x200,   "keycommand": "playplaylist&name=IR_0" },  // KEY_NUMERIC_0
    { "scancode": 0x0104,     "keycode": 0x209,   "keycommand": "play&N=8" },                // KEY_NUMERIC_9
    { "scancode": 0x0105,     "keycode": 0x208,   "keycommand": "play&N=7" },                // KEY_NUMERIC_8
    { "scancode": 0x0108,     "keycode": 0x206,   "keycommand": "play&N=5" },                // KEY_NUMERIC_6
    { "scancode": 0x0109,     "keycode": 0x205,   "keycommand": "play&N=4" },                // KEY_NUMERIC_5
    { "scancode": 0x010c,     "keycode": 0x203,   "keycommand": "play&N=2" },                // KEY_NUMERIC_3
    { "scancode": 0x10002,    "keycode": 0x203,   "keycommand": "play&N=2" },                // KEY_NUMERIC_3
    { "scancode": 0x010d,     "keycode": 0x202,   "keycommand": "play&N=1" },                // KEY_NUMERIC_2
    { "scancode": 0x10001,    "keycode": 0x202,   "keycommand": "play&N=1" },                // KEY_NUMERIC_2
    { "scancode": 0x010f,     "keycode": 0x18f,   "keycommand": "" },                        // KEY_GREEN
    { "scancode": 0x0110,     "keycode": 114,     "keycommand": "volume&volume=minus" },     // KEY_VOLUMEDOWN
    { "scancode": 0x10013,    "keycode": 114,     "keycommand": "volume&volume=minus" },     // KEY_VOLUMEDOWN
    { "scancode": 0x0111,     "keycode": 102,     "keycommand": "" },                        // KEY_HOME
    { "scancode": 0x0113,     "keycode": 0x160,   "keycommand": "" },                        // KEY_OK
    { "scancode": 0x0116,     "keycode": 103,     "keycommand": "volume&volume=plus" },      // KEY_UP
    { "scancode": 0x0118,     "keycode": 115,     "keycommand": "volume&volume=plus" },      // KEY_VOLUMEUP
    { "scancode": 0x10012,    "keycode": 115,     "keycommand": "volume&volume=plus" },      // KEY_VOLUMEUP
    { "scancode": 0x0119,     "keycode": 158,     "keycommand": "" },                        // KEY_BACK
    { "scancode": 0x011a,     "keycode": 108,     "keycommand": "volume&volume=minus" },     // KEY_DOWN
    { "scancode": 0x0140,     "keycode": 116,     "keycommand": "" },                        // KEY_POWER
    { "scancode": 0x0141,     "keycode": 113,     "keycommand": "volume&volume=toggle" },    // KEY_MUTE
    { "scancode": 0x0142,     "keycode": 14,      "keycommand": "playplaylist&name=IR_1" },  // KEY_BACKSPACE
    { "scancode": 0x0143,     "keycode": 0x18e,   "keycommand": "volume&volume=toggle" },    // KEY_RED
    { "scancode": 0x0144,     "keycode": 356,     "keycommand": "" },                        // KEY_POWER2
    { "scancode": 0x0146,     "keycode": 0x207,   "keycommand": "play&N=6" },                // KEY_NUMERIC_7
    { "scancode": 0x0147,     "keycode": 215,     "keycommand": "" },                        // KEY_EMAIL
    { "scancode": 0x014a,     "keycode": 0x204,   "keycommand": "play&N=3" },                // KEY_NUMERIC_4
    { "scancode": 0x014c,     "keycode": 139,     "keycommand": "playplaylist&name=IR_2" },  // KEY_MENU
    { "scancode": 0x014e,     "keycode": 0x201,   "keycommand": "play&N=0" },                // KEY_NUMERIC_1
    { "scancode": 0x10000,    "keycode": 0x201,   "keycommand": "play&N=0" },                // KEY_NUMERIC_1
    { "scancode": 0x0150,     "keycode": 106,     "keycommand": "next" },                    // KEY_RIGHT
    { "scancode": 0x0151,     "keycode": 105,     "keycommand": "prev" },                    // KEY_LEFT
    { "scancode": 0x0152,     "keycode": 128,     "keycommand": "stop" },                    // KEY_STOP
    { "scancode": 0x0154,     "keycode": 163,     "keycommand": "next" },                    // KEY_NEXTSONG
    { "scancode": 0x0155,     "keycode": 165,     "keycommand": "prev" },                    // KEY_PREVIOUSSONG
    { "scancode": 0x0158,     "keycode": 0x197,   "keycommand": "next" },                    // KEY_NEXT
    { "scancode": 0x0159,     "keycode": 0x19c,   "keycommand": "prev" },                    // KEY_PREVIOUS
    { "scancode": 0x015a,     "keycode": 164,     "keycommand": "toggle" },                  // KEY_PLAYPAUSE
    { "scancode": 0x10032,    "keycode": 207,     "keycommand": "play" },                    // KEY_PLAY
    { "scancode": 0x10038,    "keycode": 128,     "keycommand": "stop" },                    // KEY_STOP
    { "scancode": 0x10039,    "keycode": 164,     "keycommand": "toggle" },                  // KEY_PLAYPAUSE
    { "scancode": 0x01f1,     "keycode": 356,     "keycommand": "" },                        // KEY_POWER2
    { "scancode": 0x01f2,     "keycode": 115,     "keycommand": "volume&volume=plus" },      // KEY_VOLUMEUP
    { "scancode": 0x01f3,     "keycode": 114,     "keycommand": "volume&volume=minus" },     // KEY_VOLUMEDOWN
    { "scancode": 0xffffff,   "keycode": 255,     "keycommand": "RELEASE" }
];

// ------------------------------------------------------------
// I2C
// ------------------------------------------------------------

async function readI2cData() {

    const buffer = Buffer.alloc(DATA_LENGTH);

    try {

        const result = await i2cBus.i2cRead(
            MSPM0_ADDRESS,
            DATA_LENGTH,
            buffer
        );

        if (result.bytesRead !== DATA_LENGTH) {
            console.error(
                `Short I2C read: ${result.bytesRead} bytes`
            );
            return null;
        }

        return buffer;

    } catch (err) {

        console.error('I2C read failed:', err.message);
        return null;
    }
}

// ------------------------------------------------------------
// SEND VOLUMIO COMMAND
// ------------------------------------------------------------

async function volumioCommand(command) {
    try {
        const response = await fetch(
//            `http://localhost:3000/api/v1/commands/?cmd=${command}`
            `http://${VOLUMIO_HOST}:3000/api/v1/commands/?cmd=${command}`
        );

        if (!response.ok) {
            console.error(`Volumio command failed: ${response.status}`);
        }
    } catch (err) {
        console.error(`Volumio command failed: ${err.message}`);
    }
}

// ------------------------------------------------------------
// MSPM0 IRQ
// ------------------------------------------------------------

async function mspm0Interrupt() {

    const data = await readI2cData();

    if (data === null)
        return;

    handleScancode(data);
}

// ------------------------------------------------------------
// RETURN CODE HANDLING
// ------------------------------------------------------------

function handleScancode(data) {

    let entry = '';
    let scancode = '';

    if ((data[0] & 0x7f) === 0) {
        scancode = 0xffffff;
    } else if (data[0] & 0x60) {
        scancode = data[0]++ & 0x20 ? 0x0118 : 0x0110                              // line 165
    } else if (data[0] & 0x80) {
        if (data.readUIntBE(1,3) === 0xffffff) {
            scancode = data.readUIntBE(4,4);
        } else {
            scancode = data.readUIntBE(1,3);
        }
    }

    entry = table.find(row => row.scancode === scancode);

    if (scancode && !entry) {
        console.log(`Unknown scancode: 0x${scancode.toString(16)}`);
        return;
    }

    lastRepeat = data[0] & 0x1f;

    if (scancode) handleKeycode(entry.keycode, entry.keycommand, data[0]);
}

function handleKeycode(keycode, command, data0) {

    const now = Date.now();

    if ([28, 116].includes(keycode)) {                                             // power
        press_time = now;
        key = keycode;

        if ([0x81, 0xa0, 0xa1, 0xc0, 0xc1].includes(data0)) {                      // see line 165
            next = now + POWER_DELAY;
        } else if (now > next) {
            console.log('Poweroff');
            const power = spawn('/bin/systemctl', ['poweroff']);
            power.stderr.on('data', data => {
                console.error('Poweroff requires root privileges');
            });
            next = now + POWER_DELAY;
        }

    } else if ([255,
                512, 513, 514, 515, 516,
                517, 518, 519, 520, 521].includes(keycode)) {                      // number & release
        if (keycode == 255 && key) {
            release_time = now;
            if (key == 28) {                                                       // rotary button released within timeout
                console.log('toggle');                                             // spurious toggle after Poweroff
                volumioCommand('toggle')
            } else if (key == 116) {                                               // do nothing
                ;
            } else {                                                               // number key release
                if (release_time - press_time >= NUMBER_DELAY) {
                    console.log(`playplaylist&name=IR_${key - 0x200}`);
                    volumioCommand(`playplaylist&name=IR_${key - 0x200}`)
                    press_count = 0;
                } else {
                    press_count++;

                    if (press_count > 3) {
                        press_count = 1;
                        track_number = 0;
                    }

                    track_number = track_number * 10 + (key - 0x200);

                    if (trackTimer)
                        clearTimeout(trackTimer);

                    trackTimer = setTimeout(() => {
                        if (track_number) {
                            console.log(`play&N=${track_number - 1}`);
                            volumioCommand(`play&N=${track_number - 1}`)
                        }
                        track_number = 0;
                        press_count = 0;
                        trackTimer = null;
                    }, TRACK_DELAY);
                }
            }
            key = 0;
        } else if (data0 == 0x81) {
            press_time = now;
            key = keycode;
        }

    } else if ([103, 108, 114, 115].includes(keycode)) {                           // volume
        if ([0x81, 0xa0, 0xa1, 0xc0, 0xc1].includes(data0)) {                      // see line 165
            console.log(command);
            volumioCommand(command);
            delay = INITIAL_DELAY;
            next = now + delay;
            delay = INITIAL_DELAY;
        } else if (now > next) {
            console.log(command);
            volumioCommand(command);
            delay = (delay > MINIMUM_DELAY) ? DECAY * delay : MINIMUM_DELAY;
            next = now + delay;
        }
    } else if (data0 == 0x81 && command) {                                         // others
       console.log(command);
       volumioCommand(command);
    }
}

// ------------------------------------------------------------
// GPIO monitor
// ------------------------------------------------------------

function startGpioMonitor() {

    console.log('Monitoring GPIO' + GPIO_LINE +'...');

    gpio = spawn('gpiomon', [
        '-b',
        '-f',
        GPIO_CHIP,
        GPIO_LINE
    ]);

    gpio.stdout.on('data', data => {

        const lines = data.toString().trim().split('\n');

        for (const line of lines) {

            if (!line)
                continue;

            mspm0Interrupt();
        }
    });

    gpio.stderr.on('data', data => {
        console.error('gpiomon:', data.toString().trim());
    });

    gpio.on('close', code => {
        console.log(`gpiomon stopped (${code})`);
    });

    gpio.on('error', err => {
        console.error('gpiomon error:', err.message);
    });
}

// ------------------------------------------------------------
// Startup
// ------------------------------------------------------------

async function main() {

    try {

        i2cBus = await i2c.openPromisified(I2C_BUS);

        console.log('I2C bus opened');

        startGpioMonitor();

    } catch (err) {

        console.error('Startup failed:', err.message);
        process.exit(1);
    }
}


// ------------------------------------------------------------
// Shutdown
// ------------------------------------------------------------

process.on('SIGINT', async () => {

    console.log('\nStopping...');

    if (gpio)
        gpio.kill();

    if (i2cBus)
        await i2cBus.close();

    process.exit(0);
});

main();

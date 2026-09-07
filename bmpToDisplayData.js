const fs = require('fs');

const INPUT = 'font.h';
const OUTPUT = 'display-data.js';

const SCREEN_WIDTH = 64;
const SCREEN_HEIGHT = 48;
const PAGES = SCREEN_HEIGHT / 8;

// SSD1306 starts the 64-pixel display at column 32
const DISPLAY_X = 32;

// ------------------------------------------------------------
// Read font.h
// ------------------------------------------------------------

const source = fs.readFileSync(INPUT, 'utf8');

const images = {};

const regex = /uint8_t\s+(\w+)\[(\d+)\]\s*=\s*\{([\s\S]*?)\};/g;

let match;

while ((match = regex.exec(source)) !== null) {
    const name = match[1];
    const values = [...match[3].matchAll(/0x[0-9a-fA-F]+|\d+/g)]
        .map(m => Number(m[0]));

    images[name] = Buffer.from(values);

    console.log(`${name}: ${values.length} bytes`);
}

// ------------------------------------------------------------
// Read little-endian 32-bit integer from BMP
// ------------------------------------------------------------

function uint32le(buffer, offset) {
    return buffer.readUInt32LE(offset);
}

// ------------------------------------------------------------
// Convert one BMP to our 64x48 SSD1306 framebuffer
//
// This intentionally reproduces load_bmp_1bit() from the C
// program, including the X/Y reversal because the display
// is mounted upside down.
// ------------------------------------------------------------

function addBmp(framebuffer, bmp, xOffset, yOffset) {

    const width  = uint32le(bmp, 18);
    const height = uint32le(bmp, 22);
    const offset = uint32le(bmp, 10);

    const rowSize = Math.ceil(width / 32) * 4;

    const pixelData = offset;

    for (let y = 0; y < height; y++) {

        // BMP is stored bottom-up
        const srcY = height - 1 - y;

        const row = pixelData + srcY * rowSize;

        for (let x = 0; x < width; x++) {

            const byteIndex = Math.floor(x / 8);
            const bitIndex = 7 - (x % 8);

            const pixelOn =
                (bmp[row + byteIndex] >> bitIndex) & 1;

            // This is deliberately !pixelOn.
            // It is what the original C program does.
            if (!pixelOn) {

                // Display is physically upside down.
                const col =
                    (SCREEN_WIDTH - 1) - (x + xOffset);

                const rowY =
                    (SCREEN_HEIGHT - 1) - (y + yOffset);

                if (
                    col >= 0 &&
                    col < SCREEN_WIDTH &&
                    rowY >= 0 &&
                    rowY < SCREEN_HEIGHT
                ) {
                    const page = Math.floor(rowY / 8);
                    const bit = rowY % 8;

                    framebuffer[page * SCREEN_WIDTH + col]
                        |= 1 << bit;
                }
            }
        }
    }
}

// ------------------------------------------------------------
// Create framebuffer for a bmp_number
//
// This reproduces write_fb() from the C program.
// ------------------------------------------------------------

function makeFramebuffer(num) {

    const framebuffer =
        Buffer.alloc(SCREEN_WIDTH * PAGES, 0);

    const tens = Math.floor(num / 10);
    const ones = num % 10;

    if (tens !== 0 && num < 100) {

        const bmp = images[`number_${tens}`];

        const width = uint32le(bmp, 18);
        const height = uint32le(bmp, 22);

        addBmp(
            framebuffer,
            bmp,
            22 - width,
            Math.floor(SCREEN_HEIGHT / 2) -
                Math.floor(height / 2)
        );
    }

    if (num < 100) {

        const bmp = images[`number_${ones}`];

        const height = uint32le(bmp, 22);

        addBmp(
            framebuffer,
            bmp,
            28,
            Math.floor(SCREEN_HEIGHT / 2) -
                Math.floor(height / 2)
        );

    } else if (num >= 100 && num <= 110) {

        const bmp = [
            'number_10',
            'play_48',
            'pause_48',
            'stop_48',
            'mute_48',
            'Start',
            'Off',
            'CD_48',
            'power_48',
            'hdtv_48',
            'radio_48'
        ][num - 100];

        const image = images[bmp];

        if (!image)
            throw new Error(`Missing ${bmp} in font.h`);

        const height = uint32le(image, 22);

        addBmp(
            framebuffer,
            image,
            0,
            Math.floor(SCREEN_HEIGHT / 2) -
                Math.floor(height / 2)
        );
    }

    return framebuffer;
}

// ------------------------------------------------------------
// Turn framebuffer into complete SSD1306 I2C packets.
//
// Each packet is:
//
//   00 Bx 00 12
//   40 <64 bytes>
//
// The first four bytes position the display.
// The 0x40 byte switches to display data.
// ------------------------------------------------------------

function makeCommands(framebuffer) {

    const commands = [];

    for (let page = 0; page < PAGES; page++) {

        const packet = Buffer.alloc(4 + 1 + SCREEN_WIDTH);

        // SSD1306 command mode
        packet[0] = 0x00;

        // Page
        packet[1] = 0xb0 | page;

        // Column 32
        packet[2] = DISPLAY_X & 0x0f;
        packet[3] = 0x10 | ((DISPLAY_X >> 4) & 0x0f);

        // SSD1306 data mode
        packet[4] = 0x40;

        framebuffer.copy(
            packet,
            5,
            page * SCREEN_WIDTH,
            (page + 1) * SCREEN_WIDTH
        );

        commands.push(packet);
    }

    return commands;
}

// ------------------------------------------------------------
// Generate all possible bmp_number values
// ------------------------------------------------------------

const displays = {};

for (let num = 0; num <= 110; num++) {

    const framebuffer = makeFramebuffer(num);
    displays[num] = makeCommands(framebuffer);
}

// ------------------------------------------------------------
// Generate JavaScript
// ------------------------------------------------------------

let output = '';

output += '// Generated by convert-font.js\n';
output += '// Do not edit manually.\n\n';

output += 'const displays = {\n';

for (let num = 0; num <= 110; num++) {

    output += `    ${num}: [\n`;

    for (const packet of displays[num]) {
        output += `        Buffer.from([`;

        output += [...packet]
            .map(value => `0x${value.toString(16).padStart(2, '0')}`)
            .join(', ');

        output += `]),\n`;
    }

    output += '    ],\n';
}

output += '};\n\n';
output += 'module.exports = displays;\n';

fs.writeFileSync(OUTPUT, output);

console.log(`\nWritten ${OUTPUT}`);
console.log(`Generated ${Object.keys(displays).length} displays.`);

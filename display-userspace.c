// gcc -Wall  -Wextra -o volumio-display display-userspace.c
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#include "font.h"

#define BRIGHTNESS 127
#define SCREEN_WIDTH 64
#define SCREEN_HEIGHT 48
#define NUMBER_OF_ROWS SCREEN_HEIGHT / 8

static int file_i2c = 0;

uint8_t ssd1306_init(void)
{
    file_i2c = open("/dev/i2c-1", O_RDWR);
    if (file_i2c < 0)
        return 1;
    if (ioctl(file_i2c, I2C_SLAVE, 0x3c) < 0) // set slave address
        {
            close(file_i2c);
            return 1;
        }

    uint8_t cmd[] = {0x00, 0xae, 0xa6, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40, 0x8d, 0x14, 0x20,
        0x02, 0xa0, 0xc0, 0xda, 0x12, 0x81, BRIGHTNESS, 0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6,0xaf, 0x2e};
    uint8_t ret = write(file_i2c, &cmd, sizeof(cmd));
    if (ret <= 0) {
        close(file_i2c);
        file_i2c = 0;
        return 1;
    }
    return 0;
}

void write_pos(uint8_t x, uint8_t y)
{
    uint8_t cmd[] = {0x00, 0xb0 + y, 0x00 + (x & 0x0f), 0x10 + ((x >> 4) & 0x0f)};
    write(file_i2c, &cmd, sizeof(cmd));
}

void clear(void)
{
    uint8_t cmd[SCREEN_WIDTH + 1] = {0};
    cmd[0] = 0x40;
    for(int i = 0; i < NUMBER_OF_ROWS; i++) {
        write_pos(32, i);
        if (write(file_i2c, &cmd, sizeof(cmd)) != sizeof(cmd))
            perror("I2C write failed");
    }
}

void set_brightness(uint8_t brightness)
{
    uint8_t cmd[] = {0x00, 0x81, brightness};
    if (write(file_i2c, &cmd, sizeof(cmd)) != sizeof(cmd))
        perror("I2C write failed");
}

void load_bmp_1bit(const uint8_t *bmp_data,
                   uint8_t *i2c_data, int x_offset, int y_offset)
{
    int width  = *(uint32_t*)&bmp_data[18];
    int height = *(uint32_t*)&bmp_data[22];
    int offset = *(uint32_t*)&bmp_data[10];
    int row_size = ((width + 31) / 32) * 4;
    const uint8_t *pixel_data = bmp_data + offset;

    for (int y = 0; y < height; y++) {                 // 0 … height-1
        int src_y = height - 1 - y;                    // flip vertically
        const uint8_t *row = pixel_data + src_y * row_size;
        for (int x = 0; x < width; x++) {
            int byte_index = x / 8;
            int bit_index  = 7 - (x % 8);
            int pixel_on   = (row[byte_index] >> bit_index) & 1;
            if (!pixel_on) {
//                int col = x + x_offset;
//                int row_y = y + y_offset;
                int col = (SCREEN_WIDTH - 1) - (x + x_offset);
                int row_y = (SCREEN_HEIGHT - 1) - (y + y_offset);
                int page = row_y / 8;
                int bit  = row_y % 8;
                if (col >= 0 && col < SCREEN_WIDTH && row_y >= 0 && row_y < SCREEN_HEIGHT)
                    i2c_data[page * SCREEN_WIDTH + col] |= (1 << bit);
            }
        }
    }
}

void write_data(uint8_t x, uint8_t y, uint8_t *data, int len)
{
    write_pos(x, y);
    uint8_t cmd[len + 1];
    cmd[0] = 0x40;
    memcpy(cmd + 1, data, len);
    if (write(file_i2c, &cmd, len + 1) != len + 1)
        perror("I2C write failed");}

int main(int argc, char **argv)
{
    const uint8_t *numbers[] = {
        number_0, number_1, number_2, number_3, number_4,
        number_5, number_6, number_7, number_8, number_9,
        number_10, play_48, pause_48, stop_48, mute_48,
        Start, Off, CD_48, power_48, hdtv_48, radio_48
    };

    uint8_t num = 0;
    if (argc > 1) 
        num = atoi(argv[1]);
    int tens = num / 10;
    int ones = num % 10;

    uint8_t *data = calloc(1, SCREEN_WIDTH * NUMBER_OF_ROWS);
    if (!data) {
        perror("Failed to allocate data buffer");
        close(file_i2c);
        return 1;
    }

    ssd1306_init();
    if (tens != 0 && num < 100)
        load_bmp_1bit(numbers[tens], data,
            38 - numbers[tens][18], 24 - (numbers[tens][22] / 2));
    if (num < 100)
        load_bmp_1bit(numbers[ones], data,
            44, 24 - numbers[ones][22] / 2);
    else if (num >= 100 && num <= 110)
        load_bmp_1bit(numbers[num - 90], data,
        0, 24 - (numbers[num - 90][22] / 2));

    for(int i = 0; i < NUMBER_OF_ROWS; i++) {
        write_data(32, i, data + (i * SCREEN_WIDTH), SCREEN_WIDTH);
    }
    free(data);
    close(file_i2c);
    return 0;
}

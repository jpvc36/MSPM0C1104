// gcc -Wall -Wextra userspace-display.c -o volumio-display -lcjson -lcurl

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <cjson/cJSON.h>
//#include <linux/fb.h>

#include "font.h"

#define SOCKET_PATH "/tmp/volumio.sock"
#define I2C_DEVICE "/dev/i2c-1"
#define I2C_SLAVE_ADDRESS 0x3c
#define BRIGHTNESS 127
#define SCREEN_WIDTH 64
#define SCREEN_HEIGHT 48
#define NUMBER_OF_ROWS SCREEN_HEIGHT / 8

//#define FRAMEBUFFER "/dev/fb1"

volatile sig_atomic_t running = 1;
static int file_i2c = 0;

void handle_signal(int sig) {
    running = 0;
}

int open_file_i2c(void)
{
    file_i2c = open(I2C_DEVICE, O_RDWR);
    if (file_i2c < 0)
        return 1;
    if (ioctl(file_i2c, I2C_SLAVE, I2C_SLAVE_ADDRESS) < 0) {
        close(file_i2c);
        return 1;
    }
    return 0;
}


int setup_unix_socket() {
    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH);  // remove old

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }

    return sock;
}

uint8_t ssd1306_init(void)
{
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

void set_brightness(int brightness)
{
    uint8_t cmd[3] = {0x00, 0x81, brightness};
    if (write(file_i2c, &cmd, sizeof(cmd)) != sizeof(cmd))
        perror("I2C write failed");
}

void write_pos(uint8_t x, uint8_t y)
{
    uint8_t cmd[] = {0x00, 0xb0 + y, 0x00 + (x & 0x0f), 0x10 + ((x >> 4) & 0x0f)};
    write(file_i2c, &cmd, sizeof(cmd));
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
    if (write(file_i2c, cmd, len + 1) != len + 1)
        perror("I2C write failed");
}

int write_fb(uint8_t num) {
    // -----------------------------
    // Lookup table for numbers
    // -----------------------------
    const uint8_t *numbers[] = {
        number_0, number_1, number_2, number_3, number_4,
        number_5, number_6, number_7, number_8, number_9,
        number_10, play_48, pause_48, stop_48, mute_48,
        Start, Off, CD_48, power_48, hdtv_48, radio_48
    };

//    int width = 0, height = 0;
    int tens = num / 10;
    int ones = num % 10;

    uint8_t *data = calloc(1, SCREEN_WIDTH * NUMBER_OF_ROWS);
    if (!data) {
        perror("Failed to allocate data buffer");
        close(file_i2c);
        return 1;
    }

    if (tens != 0 && num < 100)
        load_bmp_1bit(numbers[tens], data,
//            (SCREEN_WIDTH / 2) + 6 - numbers[tens][18],
            22 - numbers[tens][18],
            (SCREEN_HEIGHT / 2) - (numbers[tens][22] / 2));
    if (num < 100)
        load_bmp_1bit(numbers[ones], data,
//            (SCREEN_WIDTH / 2) + 12,
            28,
            (SCREEN_HEIGHT / 2) - numbers[ones][22] / 2);
    else if (num >= 100 && num <= 110)
        load_bmp_1bit(numbers[num - 90], data,
        0, (SCREEN_HEIGHT / 2) - (numbers[num - 90][22] / 2));

    for(int i = 0; i < NUMBER_OF_ROWS; i++) {
        write_data(32, i, data + (i * SCREEN_WIDTH), SCREEN_WIDTH);
    }
    free(data);

    return 0;
}

void handle_volumio_event(const char *json)
{

    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
//        printf("Brightness: %d\n", brightness->valueint);
        set_brightness(brightness->valueint);
    }

    const cJSON *bmp_number = cJSON_GetObjectItemCaseSensitive(root, "bmp_number");
    if (cJSON_IsNumber(bmp_number)) {
//        printf("Volume: %d\n", bmp_number->valueint);
        write_fb(bmp_number->valueint);
    }

    cJSON_Delete(root);

}

//int main(int argc, const char *argv[])
int main(void)
{
    open_file_i2c();
    ssd1306_init();
    set_brightness(159);
    write_fb(108);

    int sockfd = setup_unix_socket();
    if (sockfd < 0) {
        fprintf(stderr, "Failed to setup UNIX socket\n");
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

fd_set readfds;
struct timeval tv;

while (running) {
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);

    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100 ms

    int ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR) continue;
        perror("select");
        break;
    }
    if (ret == 0) continue; // timeout

    if (FD_ISSET(sockfd, &readfds)) {
        char buf[2048];
        ssize_t n = read(sockfd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            handle_volumio_event(buf);
        }
    }
}

    write_fb(111);
    close(file_i2c);
    close(sockfd);
    printf("\nClean exit.\n");
    return 0;
}

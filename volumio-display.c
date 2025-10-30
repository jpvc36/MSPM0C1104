// gcc volumio-display.c -o volumio-display -lcjson -lcurl

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <linux/fb.h>
#include <cjson/cJSON.h>

#include "font.h"

#define SOCKET_PATH "/tmp/volumio.sock"
#define FRAMEBUFFER "/dev/fb1"

volatile sig_atomic_t running = 1;

void handle_signal(int sig) {
    running = 0;
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

void set_sysfs_brightness(int value) {
    FILE *f = fopen("/sys/class/backlight/ssd1307fb1/brightness", "w");
    if (!f) {
        perror("Failed to open brightness sysfs node");
        return;
    }
    fprintf(f, "%d\n", value);
    fclose(f);
}

uint8_t *load_bmp_1bit(const uint8_t *bmp_data,
                       uint8_t *fb_data, int x_offset, int y_offset)
{
    int row_size = ((bmp_data[18] + 31) / 32) * 4;
    const uint8_t *pixel_data = bmp_data + bmp_data[10];

    for (int y = 0; y < bmp_data[22]; y++) {				// count up to height
        int src_y = bmp_data[22] - 1 - y;
        const uint8_t *row = pixel_data + y * row_size;

        for (int x = 0; x < bmp_data[18]; x++) {				// count up to width
            int byte_index = x / 8;				// count up
            int bit_index = 7 - (x % 8);			// count back from 7
            int pixel_on = (row[byte_index] >> bit_index) & 1;

            // For SSD1306: 0 = ON (black pixel)
            if (!pixel_on) {
                fb_data[(src_y + y_offset) * 12 + ((x + x_offset) / 8)] |= 1 << ((x + x_offset) % 8);
            }
        }
    }

    return fb_data;
}

int write_fb(uint8_t num) {
    int fb = open("/dev/fb1", O_RDWR);
    if (fb < 0) {
        perror("Failed to open framebuffer");
        return 1;
    }

    int buffer_size = 576;
    uint8_t *bitmap = calloc(1, buffer_size);
    if (!bitmap) {
        perror("Failed to allocate framebuffer buffer");
        close(fb);
        return 1;
    }

    // -----------------------------
    // Lookup table for numbers
    // -----------------------------
    const uint8_t *numbers[] = {
        number_0, number_1, number_2, number_3, number_4,
        number_5, number_6, number_7, number_8, number_9,
        number_10, play_48, pause_48, stop_48, mute_48,
        Start, Off, CD_48, power_48, hdtv_48, radio_48
    };

    int width = 0, height = 0;
    int tens = num / 10;
    int ones = num % 10;

    for (int i = 0; i < 576; i++) bitmap[i] = 0;
    if (tens != 0 && num < 100)
        load_bmp_1bit(numbers[tens], bitmap,
            52 - numbers[tens][18], 24 - (numbers[tens][22] / 2));
    if (num < 100)
        load_bmp_1bit(numbers[ones], bitmap,
            58, 24 - numbers[ones][22] / 2);
    else if (num >= 100 && num <= 110) load_bmp_1bit(numbers[num - 90],
//        bitmap, 64 - numbers[num - 90][18] / 2, 24 - (numbers[num - 90][22] / 2));
        bitmap, 32, 24 - (numbers[num - 90][22] / 2));
    else {
        lseek(fb, 0, SEEK_SET);
        write(fb, bitmap, buffer_size);
        close(fb);
        free(bitmap);
        if (num > 111) fprintf(stderr, "Error: number %d is out of range (0-111)\n", num);
        return(EINVAL);
    }

    // -----------------------------
    // Write to framebuffer
    // -----------------------------

    lseek(fb, 0, SEEK_SET);
    write(fb, bitmap, buffer_size);
    close(fb);
    free(bitmap);

    return 0;
}

void handle_volumio_event(const char *json)
{

    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
//        printf("Brightness: %d\n", brightness->valueint);
        set_sysfs_brightness(brightness->valueint);
    }

    const cJSON *bmp_number = cJSON_GetObjectItemCaseSensitive(root, "bmp_number");
    if (cJSON_IsNumber(bmp_number)) {
//        printf("Volume: %d\n", bmp_number->valueint);
        write_fb(bmp_number->valueint);
    }

    cJSON_Delete(root);

}

int main(int argc, const char *argv[])
{
    system("/usr/bin/dtoverlay ssd1306 inverted width=96 height=48");

    set_sysfs_brightness(159);
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
//            printf("%s\n", buf);
        }
    }
}

    write_fb(111);
    close(sockfd);
    system("/usr/bin/dtoverlay -r ssd1306");
    printf("\nClean exit.\n");
    return 0;
}
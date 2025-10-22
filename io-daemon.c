// sudo apt install libcurl4=7.64.0-4+deb10u9 libcurl4-openssl-dev=7.64.0-4+deb10u9 libgpiod-dev libcjson-dev
// gcc io-daemon.c -o io-daemon -lgpiod -lcjson -lcurl
// dtoverlay=ssd1306,inverted,width=96,height=48
// sudo dtoverlay ssd1306 inverted width=96 height=48
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <signal.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <gpiod.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "font.h"  // contains: number_0 ... number_10 and other BMP data arrays
#include "keycode_lookup.h"

#define _POSIX_C_SOURCE 200809L
#define KEYMAP_FILE "myir.keymap.json"
#define MAX_KEYS 64
#define MAX_BUTTONS 64
#define I2C_ADDRESS 0x77
#define SOCKET_PATH "/tmp/volumio.sock"  // Adjust if needed
#define GPIO_CHIP "/dev/gpiochip0"
#define GPIO_LINE 4
#define FRAMEBUFFER "/dev/fb1"

typedef struct {
    uint32_t scancode;
    int keycode;
    char *keycommand;
} keymap_t;

keymap_t ir_table[MAX_KEYS + 1];
keymap_t btn_table[MAX_BUTTONS + 1];
int ir_keycount = 0, btn_keycount = 0;

volatile sig_atomic_t running = 1;
uint8_t i2c_data[8];
uint16_t key_code = 0xffff, last_key_code = 0xffff;
char *command_code;

// -----------------------------
// Sysfs brightness control
// -----------------------------
void set_sysfs_brightness(int value) {
    FILE *f = fopen("/sys/class/backlight/ssd1307fb1/brightness", "w");
    if (!f) {
        perror("Failed to open brightness sysfs node");
        return;
    }
    fprintf(f, "%d\n", value);
    fclose(f);
}

// -----------------------------
// Inline BMP loader (from memory)
// -----------------------------
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

bool load_keymap_section(const char *filename, const char *section_name, keymap_t *table, int *count, int max_entries) {
    struct stat st;
    if (stat(filename, &st) < 0) {
        perror("stat keymap");
        return false;
    }
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("fopen keymap");
        return false;
    }

    char *json_text = malloc(st.st_size + 1);
    if (!json_text) {
        fclose(f);
        return false;
    }
    fread(json_text, 1, st.st_size, f);
    json_text[st.st_size] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        fprintf(stderr, "Failed to parse JSON.\n");
        free(json_text);
        return false;
    }

    cJSON *section = cJSON_GetObjectItem(root, section_name);
    if (!section || !cJSON_IsArray(section)) {
        fprintf(stderr, "Section \"%s\" not found or not an array.\n", section_name);
        cJSON_Delete(root);
        free(json_text);
        return false;
    }

    *count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, section) {
        cJSON *s = cJSON_GetObjectItem(item, "scancode");
        cJSON *k = cJSON_GetObjectItem(item, "keycode");
        cJSON *v = cJSON_GetObjectItem(item, "keycommand");
        if (cJSON_IsString(s) && cJSON_IsString(k) && cJSON_IsString(v)) {
            uint32_t sc = (uint32_t)strtoul(s->valuestring, NULL, 16);
            uint16_t kc = resolve_keycode(k->valuestring);
            char* vc = v->valuestring;
            if (kc > 0 && *count < max_entries) {
                table[(*count)++] = (keymap_t){sc, kc, strdup(vc)};
            }
        }
    }

    table[*count] = (keymap_t){0, 0, 0};
    cJSON_Delete(root);
    free(json_text);
    printf("Loaded %d entries from section '%s'\n", *count, section_name);
    return *count > 0;
}

static void free_keymap(keymap_t *table, int count)
{
    for (int i = 0; i < count; ++i) {
        free(table[i].keycommand);
        table[i].keycommand = NULL;
    }
}

int lookup_code(uint32_t code, keymap_t *table, int count) {
    for (int i = 0; i < count; i++) {
        if (table[i].scancode == code)
            return table[i].keycode;
    }
    return -1;
}

char* lookup_command(uint16_t code, keymap_t *table, int count) {
    for (int i = 0; i < count; i++) {
        if (table[i].keycode == code)
            return table[i].keycommand;
    }
    return 0;
}

uint32_t get_scancode(uint8_t *buf) {
    return (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

uint32_t get_buttoncode(uint8_t *buf) {
    return (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
}

void send_volumio_command(const char *cmd) {
    CURL *curl = curl_easy_init();
    if (curl) {
        char url[256];
        snprintf(url, sizeof(url), "http://localhost:3000/api/v1/commands/?cmd=%s", cmd);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);  // optional, if you just want to send without reading body
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    }
}

int read_i2c_data(uint8_t *data, size_t len) {
    int fd;
    char *device = "/dev/i2c-1";

    if ((fd = open(device, O_RDWR)) < 0) {
        perror("Failed to open the bus");
        return -1;
    }

    struct i2c_msg msgs[1];
    msgs[0].addr  = I2C_ADDRESS;
    msgs[0].flags = I2C_M_RD;
    msgs[0].len   = len;
    msgs[0].buf   = data;

    struct i2c_rdwr_ioctl_data rdwr_data;
    rdwr_data.msgs  = msgs;
    rdwr_data.nmsgs = 1;

    if (ioctl(fd, I2C_RDWR, &rdwr_data) < 0) {
        perror("I2C_RDWR ioctl failed");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

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

/* ---------- Volumio JSON handler ---------- */
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

/* ---------- GPIO handler ---------- */
//char* handle_gpio_trigger(uint16_t *key)
uint16_t handle_gpio_trigger(void)
{
    read_i2c_data(i2c_data, 8);
    uint32_t scancode = get_scancode(i2c_data);
    uint32_t buttoncode = get_buttoncode(i2c_data);
//    *key = lookup_code(scancode, ir_table, ir_keycount);
//    return lookup_command(scancode, ir_table, ir_keycount);
    if(scancode != 0x00ffffff)return lookup_code(scancode, ir_table, ir_keycount);
    else if (buttoncode != 0) return 164;
    else if (i2c_data[0] & 0x60) return (i2c_data[0] & 0x20) ? 115 : 114;
    else return 0;
}

/* ---------- Main ---------- */
int main(int argc, const char *argv[])
{
    set_sysfs_brightness(159);
    write_fb(108);
    const char *ir_section = "default";  // default fallback
    if (argc > 1) {
        ir_section = argv[1];  // first argument is the IR section
    }

    if (!load_keymap_section(KEYMAP_FILE, ir_section, ir_table, &ir_keycount, MAX_KEYS) ||
        !load_keymap_section(KEYMAP_FILE, "Button", btn_table, &btn_keycount, MAX_BUTTONS)) {
        fprintf(stderr, "Failed to load keymap sections.\n");
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int sockfd = setup_unix_socket();
    if (sockfd < 0) {
        fprintf(stderr, "Failed to setup UNIX socket\n");
        return 1;
    }

    /* Configure GPIO line using libgpiod */
    struct gpiod_chip *chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        perror("gpiod_chip_open");
        return 1;
    }

    struct gpiod_line *line = gpiod_chip_get_line(chip, GPIO_LINE);
    if (!line) {
        perror("gpiod_chip_get_line");
        gpiod_chip_close(chip);
        return 1;
    }

    if (gpiod_line_request_falling_edge_events(line, "volumio-daemon") < 0) {
        perror("gpiod_line_request_falling_edge_events");
        gpiod_chip_close(chip);
        return 1;
    }

    /* Get GPIO event file descriptor */
    int gpio_fd = gpiod_line_event_get_fd(line);
    if (gpio_fd < 0) {
        perror("gpiod_line_event_get_fd");
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        return 1;
    }

    printf("Monitoring UNIX socket (%s) and GPIO line %d...\n", SOCKET_PATH, GPIO_LINE);

    /* Setup poll() */
    struct pollfd fds[2];
    memset(fds, 0, sizeof(fds));

    fds[0].fd = sockfd;
    fds[0].events = POLLIN;

    fds[1].fd = gpio_fd;
    fds[1].events = POLLIN;

    while (running) {
//        int ret = poll(fds, 2, -1);  // Wait indefinitely
        int ret = poll(fds, 2, 100);  // Wait 100ms
        if (ret < 0) {
            if (errno == EINTR) continue; // Interrupted by signal
            perror("poll");
            break;
        }

        /* --- Volumio UNIX socket event --- */
        if (fds[0].revents & POLLIN) {
            char buf[2048];
            ssize_t n = read(sockfd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                handle_volumio_event(buf);
            }
        }

        /* --- GPIO edge event --- */
        if (fds[1].revents & POLLIN) {
            struct gpiod_line_event event;
            if (gpiod_line_event_read(line, &event) == 0) {
                if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
                    key_code = handle_gpio_trigger();
                    char *command_code = lookup_command(key_code, ir_table, ir_keycount);
                    if(key_code == 65535);// printf("RELEASE\n");
                    else if(last_key_code != key_code) {
//                        printf("Keycode %d, Command %s\n", key_code, command_code);
                        send_volumio_command(command_code);
                    }
                }
            }
        last_key_code = key_code;
        }
    }

    /* Cleanup */
    gpiod_line_release(line);
    gpiod_chip_close(chip);
    close(sockfd);
    free_keymap(ir_table, ir_keycount);
    free_keymap(btn_table, btn_keycount);

    write_fb(111);
    printf("Clean exit.\n");
    return 0;
}


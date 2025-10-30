// sudo apt install -y libgpiod-dev libcjson-dev libcurl4-openssl-dev
// gcc -Wall -Wextra -O2 -o 1104-volumio 1104-volumio.c -lgpiod -lcurl -lcjson -lrt (-lrt for older system)
// gpioinfo gpiochip0

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <gpiod.h>
#include <time.h>
#include <cjson/cJSON.h>
#include "keycode_lookup.h"
#include <curl/curl.h>
#include <syslog.h>

#define MAX_KEYS 64
#define MAX_BUTTONS 64
#define KEYMAP_FILE "myir.keymap.json"
#define INITIAL_DELAY 500		// ms for volume delay
#define MINIMUM_DELAY 100		// ms for volume delay
#define DECAY .85			// for volume delay
#define KEY_REPEAT_DELAY_MS 1000	// for number keys

uint8_t i2c_data[8];			// Buffer to store 8 bytes
typedef struct {
    uint32_t scancode;
    uint16_t keycode;
    char *keycommand;
} keymap_t;

volatile sig_atomic_t running = 1;
keymap_t ir_table[MAX_KEYS + 1];
keymap_t btn_table[MAX_BUTTONS + 1];
int ir_keycount = 0, btn_keycount = 0;
uint16_t delay = INITIAL_DELAY;
struct gpiod_line_event event;
struct gpiod_line *line;
struct gpiod_chip *chip;

timer_t debounce_timer;
uint8_t press_count = 0;
uint16_t track_number = 0;

void send_volumio_command(const char *cmd) {
    if (!cmd || !*cmd) {
        // Nothing to send; avoid crash
        fprintf(stderr, "send_volumio_command: NULL or empty command ignored\n");
        return;
    }

    CURL *curl = curl_easy_init();
    if (curl) {
        char url[256];
        snprintf(url, sizeof(url),
                 "http://localhost:3000/api/v1/commands/?cmd=%s", cmd);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);  // we don't need body
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
    } else {
        fprintf(stderr, "curl_easy_init failed\n");
    }
}

void debounce_timeout() {
    // Example action: build a command and send
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "play&N=%d", track_number - 1);
    fprintf(stderr, "No new press for %d ms, executing action for key %03d (%d %s), %s\n",
        KEY_REPEAT_DELAY_MS, track_number, press_count + 1, press_count ? "presses" : "press", cmd);

    send_volumio_command(cmd);

    press_count = 0;      // reset for next series
    track_number = 0;
}

void setup_debounce_timer(void) {
    struct sigevent sev = {0};
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = debounce_timeout;
    if (timer_create(CLOCK_MONOTONIC, &sev, &debounce_timer) != 0) {
        perror("timer_create");
    }
}

void restart_debounce_timer(struct itimerspec its) {
    if (timer_settime(debounce_timer, 0, &its, NULL) != 0) {
        perror("timer_settime");
    }
}

int open_gpiod_line(uint8_t gpio) {
    int ret;

    // Open GPIO chip
    chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) {
        perror("Open chip failed");
        return 1;
    }

    // Get GPIO line (pin 4)
    line = gpiod_chip_get_line(chip, gpio);
    if (!line) {
        perror("Get line failed");
        gpiod_chip_close(chip);
        return 1;
    }

    // Request falling edge detection
    ret = gpiod_line_request_falling_edge_events(line, "i2c_trigger");
    if (ret < 0) {
        perror("Request event failed");
        gpiod_chip_close(chip);
        return 1;
    }
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
    size_t got = fread(json_text, 1, st.st_size, f);
        if (got != (size_t)st.st_size) {
            fprintf(stderr, "fread size mismatch: expected %zu got %zu\n", (size_t)st.st_size, got);
            free(json_text);
            fclose(f);
            return false;
        }
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

uint16_t lookup_code(uint32_t code, keymap_t *table, int count) {
    for (int i = 0; i < count; i++) {
        if (table[i].scancode == code)
            return table[i].keycode;
    }
    return -1;
}

char* lookup_command(uint32_t code, keymap_t *table, int count) {
    for (int i = 0; i < count; i++) {
        if (table[i].scancode == code)
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

void process_ir(uint32_t scan_code) {
    uint16_t keycode = lookup_code(scan_code, ir_table, ir_keycount);
    char* keycommand = lookup_command(scan_code, ir_table, ir_keycount);
    static struct timespec now, ts_next;
    clock_gettime(CLOCK_REALTIME, &now);

//printf("Scancode 0x%03x Keycode 0x%03x\n", scan_code, keycode);
    if (keycode == 114 || keycode == 115 || keycode == 103 || keycode == 108) {
        if ((i2c_data[0] & 0x1f) == 0x01) {
            send_volumio_command(keycommand);
            delay = INITIAL_DELAY;
            ts_next.tv_sec = now.tv_sec; ts_next.tv_nsec = now.tv_nsec + (delay * 1e6);
            if (ts_next.tv_nsec >= 1e9) {
                ts_next.tv_sec++;
                ts_next.tv_nsec -= 1e9;
            }
        }
        else if (now.tv_sec > ts_next.tv_sec ||
            (now.tv_sec == ts_next.tv_sec && now.tv_nsec > ts_next.tv_nsec)) {
            send_volumio_command(keycommand);
            delay = (delay > MINIMUM_DELAY) ? DECAY * delay : MINIMUM_DELAY;
            ts_next.tv_sec = now.tv_sec; ts_next.tv_nsec = now.tv_nsec + (delay * 1e6);
            if (ts_next.tv_nsec >= 1e9) {
                ts_next.tv_sec++;
                ts_next.tv_nsec -= 1e9;
            }
        }
    }
    else if (keycode == 116) {
        if ((i2c_data[0] & 0x1f) == 0x01) {
            ts_next.tv_sec = now.tv_sec; ts_next.tv_nsec = now.tv_nsec + 8e8;
            if (ts_next.tv_nsec >= 1e9) {
                ts_next.tv_sec++;
                ts_next.tv_nsec -= 1e9;
            }
        }
        else if (now.tv_sec > ts_next.tv_sec ||
            (now.tv_sec == ts_next.tv_sec && now.tv_nsec > ts_next.tv_nsec)) {
            system("/sbin/poweroff");
        }
    }
    else if ((keycode >= 0x200 && keycode <= 0x209) || scan_code == 0xffffff) {
        static uint16_t key;
        static struct timespec press_time, release_time, last_release_time;
 
        if ((i2c_data[0] & 0x1f) == 0x01) {			// just pressed
            key = keycode;					// remember keycode
            clock_gettime(CLOCK_REALTIME, &press_time);
        } else if (scan_code == 0xffffff && key) {		// just released
            clock_gettime(CLOCK_REALTIME, &release_time);

            uint32_t diff_ms = (release_time.tv_sec - press_time.tv_sec) * 1e3 +
                (release_time.tv_nsec - press_time.tv_nsec) / 1e6;

            uint32_t since_last_ms = (release_time.tv_sec - last_release_time.tv_sec) * 1e3 +
                (release_time.tv_nsec - last_release_time.tv_nsec) / 1e6;

            if (diff_ms >= 800) {				// Long press, act immediately
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "playplaylist&name=IR_%d", key - 0x200);
                send_volumio_command(cmd);
                printf("long press, executing action for key %03d, %s\n", key - 0x200, cmd);
                press_count = 0;
            }
            else {						// Short press, might be single or double
                if(since_last_ms < 1000) {
                    press_count++;
                }
                else {
                    press_count = 0;
                }
                track_number *= 10;						// shift earlier keypress left for tracks up to 999
                if(press_count % 3)						// start over when more than 3 short keypresses
                    track_number += key - 512;			// remember which key caused it
                else track_number = key - 512;			// max 3 figures
//                printf("press no %d, key %03d, %d ms, track %d\n", press_count + 1, key - 512, since_last_ms, track_number);

                last_release_time = release_time;

                // (Re)start the 500 ms timer — resets each press
                struct itimerspec its = {0};
                its.it_value.tv_sec = KEY_REPEAT_DELAY_MS / 1000;
                its.it_value.tv_nsec = (KEY_REPEAT_DELAY_MS % 1000) * 1000000;
                if (timer_settime(debounce_timer, 0, &its, NULL) != 0)
                    perror("timer_settime");
            }
        key = 0;						// reset
        }
    }
    else if ((i2c_data[0] & 0x1f) == 1) {			// other keycommands set in KEYMAP_FILE
        send_volumio_command(keycommand);
    }
}

int read_i2c_data(void) {
    int fd;
    char *device = "/dev/i2c-1";  // Bus 1 for Linux

    if ((fd = open(device, O_RDWR)) < 0) {
        perror("Failed to open the bus");
        return -1;
    }

    struct i2c_msg msgs[1];
    msgs[0].addr = 0x77;
    msgs[0].flags = I2C_M_RD;
    msgs[0].len = 8;
    msgs[0].buf = i2c_data;

    struct i2c_rdwr_ioctl_data rdwr_data;
    rdwr_data.msgs = msgs;
    rdwr_data.nmsgs = 1;

    if (ioctl(fd, I2C_RDWR, &rdwr_data) < 0) {
        perror("I2C_RDWR ioctl failed");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

void handle_signal(void) {
    running = 0;
}

static void free_keymap(keymap_t *table, int count)
{
    for (int i = 0; i < count; ++i) {
        free(table[i].keycommand);
        table[i].keycommand = NULL;
    }
}

int main(int argc, const char *argv[]) {
    int ret = 0;
    uint32_t scancode = 0, buttoncode = 0;
    const char *ir_section = "default";

    if (argc > 1) ir_section = argv[1];

    setup_debounce_timer();

    /* Initialize curl globally */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "curl_global_init failed\n");
        // Not fatal but log
    }

    bool ir_loaded = false, btn_loaded = false;
    bool chip_opened = false;

    if (!load_keymap_section(KEYMAP_FILE, ir_section, ir_table, &ir_keycount, MAX_KEYS)) {
        fprintf(stderr, "Failed to load IR keymap section '%s'\n", ir_section);
        ret = 1;
        goto cleanup;
    }
    ir_loaded = true;

    if (!load_keymap_section(KEYMAP_FILE, "Button", btn_table, &btn_keycount, MAX_BUTTONS)) {
        fprintf(stderr, "Failed to load Button keymap section\n");
        ret = 1;
        goto cleanup;
    }
    btn_loaded = true;

    if (open_gpiod_line(4)) {
        ret = 1;
        goto cleanup;
    }
    chip_opened = true;

    printf("Waiting for falling edge on GPIO4...\n");

    while (running) {
        ret = gpiod_line_event_wait(line, &(struct timespec){1, 0});
        if (ret < 0) {
            perror("Wait for event failed");
            break;
        } else if (ret > 0) {
            ret = gpiod_line_event_read(line, &event);
            if (ret == 0) {
                struct timespec small_delay = {0, 5000}; // 5us delay
                nanosleep(&small_delay, NULL);
                if (read_i2c_data() < 0) {
                    fprintf(stderr, "read_i2c_data failed\n");
                    break;
                }
                scancode = get_scancode(i2c_data);
                buttoncode = get_buttoncode(i2c_data);
            }

            if (i2c_data[0] & 0x20) {
                send_volumio_command("volume&volume=plus");
            } else if (i2c_data[0] & 0x40) {
                send_volumio_command("volume&volume=minus");
            } else if (buttoncode == 0x00000000) {
                process_ir(scancode);
            } else {
                char* buttoncommand = lookup_command(buttoncode, btn_table, btn_keycount);
                if ((i2c_data[0] & 0x1f) == 1 && buttoncommand)
                    send_volumio_command((const char*)buttoncommand);
            }
        }
    }

cleanup:
    if (ir_loaded) free_keymap(ir_table, ir_keycount);
    if (btn_loaded) free_keymap(btn_table, btn_keycount);

    if (chip_opened) {
        gpiod_chip_close(chip);
        chip_opened = false;
    }

    curl_global_cleanup();

    fprintf(stderr, "Clean exit from 1104-volumio.\n");

    return ret;
}



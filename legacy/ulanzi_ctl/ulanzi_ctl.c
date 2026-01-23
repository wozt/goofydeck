#define _POSIX_C_SOURCE 199309L
// Ulanzi D200 CLI driver using hidapi
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <hidapi/hidapi.h>

#define VID 0x2207
#define PID 0x0019
#define PACKET_SIZE 1024
#define HEADER0 0x7c
#define HEADER1 0x7c

enum CommandProtocol {
    OUT_SET_BUTTONS = 0x0001,
    OUT_SET_SMALL_WINDOW_DATA = 0x0006,
    OUT_SET_BRIGHTNESS = 0x000a,
    OUT_SET_LABEL_STYLE = 0x000b,
    OUT_PARTIALLY_UPDATE_BUTTONS = 0x000d,
    IN_BUTTON = 0x0101,
    IN_BUTTON_2 = 0x0102,
    IN_DEVICE_INFO = 0x0303
};

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void usage() {
    fprintf(stderr,
            "Usage: ulanzi_ctl <command> [options]\n"
            "Commands:\n"
            "  set-buttons --zip <file.zip> [--device-path <path>]\n"
            "  set-brightness <0-100> [--device-path <path>]\n"
            "  set-small-window [--mode N] [--cpu N] [--mem N] [--gpu N] [--time HH:MM:SS] [--device-path <path>]\n"
            "  set-label-style --json <file> [--device-path <path>]\n"
            "  ping [--device-path <path>]\n"
            "  keep-alive [--interval sec] [--device-path <path>]\n"
            "  read-buttons [--device-path <path>] [--window-ms N] [--sleep-ms N]\n");
}

static int debug_enabled() {
    const char *env = getenv("ULANZI_CTL_DEBUG");
    return (env && *env == '1');
}

static hid_device *open_device(const char *path) {
    hid_device *dev = NULL;
    if (path && *path) {
        if (debug_enabled()) fprintf(stderr, "[debug] open by path: %s\n", path);
        dev = hid_open_path(path);
    } else {
        struct hid_device_info *info = hid_enumerate(VID, PID);
        if (debug_enabled()) {
            for (struct hid_device_info *p = info; p; p = p->next) {
                fprintf(stderr, "[debug] enumerate path=%s iface=%d usage_page=%u\n", p->path ? p->path : "(null)", p->interface_number, p->usage_page);
            }
        }
        if (info) {
            dev = hid_open_path(info->path);
            hid_free_enumeration(info);
        }
        if (!dev) {
            dev = hid_open(VID, PID, NULL);
        }
    }
    if (!dev) {
        fprintf(stderr, "Failed to open device (path=%s)\n", path ? path : "auto");
    }
    return dev;
}

static int write_packet(hid_device *dev, const uint8_t *packet, size_t len) {
    uint8_t buf_with_report[PACKET_SIZE + 1];
    buf_with_report[0] = 0x00;
    memcpy(buf_with_report + 1, packet, len);
    int res = hid_write(dev, buf_with_report, len + 1);
    if (res < 0) {
        res = hid_write(dev, packet, len);
    }
    if (res < 0 && debug_enabled()) {
        const wchar_t *err = hid_error(dev);
        fprintf(stderr, "[debug] hid_write failed: %d (%ls)\n", res, err ? err : L"?");
    }
    return res;
}

static void build_packet(uint16_t command, const uint8_t *data, size_t data_len, size_t total_len, uint8_t *out) {
    memset(out, 0, PACKET_SIZE);
    out[0] = HEADER0;
    out[1] = HEADER1;
    out[2] = (command >> 8) & 0xff;
    out[3] = command & 0xff;
    out[4] = (uint8_t)(total_len & 0xff);
    out[5] = (uint8_t)((total_len >> 8) & 0xff);
    out[6] = (uint8_t)((total_len >> 16) & 0xff);
    out[7] = (uint8_t)((total_len >> 24) & 0xff);
    if (data && data_len > 0) {
        memcpy(out + 8, data, data_len > (PACKET_SIZE - 8) ? (PACKET_SIZE - 8) : data_len);
    }
}

static int send_command(hid_device *dev, uint16_t cmd, const uint8_t *data, size_t len) {
    uint8_t packet[PACKET_SIZE];
    build_packet(cmd, data, len, len, packet);
    int res = write_packet(dev, packet, PACKET_SIZE);
    return res;
}

static void patch_invalid_bytes(uint8_t *buf, size_t len) {
    const uint8_t invalid0 = 0x00;
    const uint8_t invalid1 = 0x7c;
    for (size_t i = 1016; i < len; i += 1024) {
        if (buf[i] == invalid0 || buf[i] == invalid1) {
            buf[i] = 0x01;
        }
    }
}

static int send_file(hid_device *dev, const uint8_t *data, size_t len) {
    uint8_t *patched = malloc(len);
    if (!patched) die("malloc");
    memcpy(patched, data, len);
    patch_invalid_bytes(patched, len);

    uint8_t packet[PACKET_SIZE];
    size_t first_len = PACKET_SIZE - 8;
    build_packet(OUT_SET_BUTTONS, patched, first_len, len, packet);
    int res = write_packet(dev, packet, PACKET_SIZE);
    if (res < 0) { free(patched); return res; }

    size_t offset = first_len;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > PACKET_SIZE) chunk = PACKET_SIZE;
        uint8_t buf[PACKET_SIZE];
        memset(buf, 0, PACKET_SIZE);
        memcpy(buf, patched + offset, chunk);
        res = write_packet(dev, buf, PACKET_SIZE);
        if (res < 0) { free(patched); return res; }
        offset += chunk;
    }
    free(patched);
    return 0;
}

static int cmd_set_buttons(const char *zip_path, const char *device_path) {
    FILE *f = fopen(zip_path, "rb");
    if (!f) { perror("zip open"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); fprintf(stderr, "zip empty\n"); return 1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) die("malloc zip");
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("zip read"); free(buf); fclose(f); return 1; }
    fclose(f);

    hid_device *dev = open_device(device_path);
    if (!dev) { free(buf); return 1; }
    hid_set_nonblocking(dev, 0);
    int res = send_file(dev, buf, (size_t)sz);
    free(buf);
    hid_close(dev);
    if (res < 0) {
        fprintf(stderr, "send_file failed: %d\n", res);
        return 1;
    }
    printf("Sent ZIP (%ld bytes)\n", sz);
    return 0;
}

static int cmd_set_brightness(int val, const char *device_path) {
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    char payload[16];
    snprintf(payload, sizeof(payload), "%d", val);
    hid_device *dev = open_device(device_path);
    if (!dev) return 1;
    hid_set_nonblocking(dev, 0);
    int res = send_command(dev, OUT_SET_BRIGHTNESS, (const uint8_t *)payload, strlen(payload));
    hid_close(dev);
    if (res < 0) { fprintf(stderr, "set_brightness failed\n"); return 1; }
    printf("Set brightness to %d\n", val);
    return 0;
}

static int cmd_set_small_window(int mode, int cpu, int mem, int gpu, const char *time_str, const char *device_path) {
    char payload[64];
    snprintf(payload, sizeof(payload), "%d|%d|%d|%s|%d", mode, cpu, mem, time_str, gpu);
    hid_device *dev = open_device(device_path);
    if (!dev) return 1;
    hid_set_nonblocking(dev, 0);
    int res = send_command(dev, OUT_SET_SMALL_WINDOW_DATA, (const uint8_t *)payload, strlen(payload));
    if (res < 0) {
        const wchar_t *err = hid_error(dev);
        fprintf(stderr, "set_small_window failed (%ls)\n", err ? err : L"?");
        hid_close(dev);
        return 1;
    }
    hid_close(dev);
    printf("Set small window: %s\n", payload);
    return 0;
}

static int cmd_set_label_style(const char *json_path, const char *device_path) {
    FILE *f = fopen(json_path, "rb");
    if (!f) { perror("json open"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) { fclose(f); fprintf(stderr, "json invalid size\n"); return 1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) die("malloc json");
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("json read"); free(buf); fclose(f); return 1; }
    buf[sz] = '\0';
    fclose(f);

    hid_device *dev = open_device(device_path);
    if (!dev) { free(buf); return 1; }
    hid_set_nonblocking(dev, 0);
    int res = send_command(dev, OUT_SET_LABEL_STYLE, (const uint8_t *)buf, strlen(buf));
    if (res < 0) {
        const wchar_t *err = hid_error(dev);
        fprintf(stderr, "set_label_style failed (%ls)\n", err ? err : L"?");
        free(buf);
        hid_close(dev);
        return 1;
    }
    free(buf);
    hid_close(dev);
    printf("Set label style from %s\n", json_path);
    return 0;
}

static void wake_device(hid_device *dev) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    char payload[64];
    snprintf(payload, sizeof(payload), "%d|%d|%d|%s|%d", 1, 0, 0, buf, 0);
    send_command(dev, OUT_SET_SMALL_WINDOW_DATA, (const uint8_t *)payload, strlen(payload));
}

static int cmd_ping(const char *device_path) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    return cmd_set_small_window(1, 0, 0, 0, buf, device_path);
}

static int cmd_keep_alive(int interval, const char *device_path) {
    printf("Keep-alive every %d seconds (Ctrl+C to stop)\n", interval);
    while (1) {
        int res = cmd_ping(device_path);
        (void)res;
        sleep(interval);
    }
    return 0;
}

static int cmd_read_buttons(const char *device_path) {
    hid_device *dev = open_device(device_path);
    if (!dev) return 1;
    hid_set_nonblocking(dev, 0);
    // Wake device so it emits events even when UI is blank
    wake_device(dev);
    time_t last_wake = time(NULL);
    double down_time[14] = {0};
    int hold_emitted[14] = {0};
    int tap_pending[14] = {0};
    const double HOLD_THRESHOLD = 0.75; // seconds
    const double TAP_THRESHOLD = 0.02;  // seconds
    printf("Listening for button events (Ctrl+C to stop)...\n");
    while (1) {
        uint8_t buf[PACKET_SIZE];
        int res = hid_read_timeout(dev, buf, sizeof(buf), 500);
        if (res > 0) {
            if (buf[0]==HEADER0 && buf[1]==HEADER1) {
                uint16_t cmd = ((uint16_t)buf[2] << 8) | buf[3];
                if (cmd==IN_BUTTON || cmd==IN_BUTTON_2) {
                    uint8_t state = buf[8];
                    uint8_t index = buf[9];
                    uint8_t pressed = buf[11] == 0x01;
                    struct timespec ts_now;
                    clock_gettime(CLOCK_MONOTONIC, &ts_now);
                    double now = (double)ts_now.tv_sec + (double)ts_now.tv_nsec / 1e9;
                    if (pressed) {
                        if (down_time[index] == 0) {
                            down_time[index] = now;
                            hold_emitted[index] = 0;
                            tap_pending[index] = 1;
                        }
                    } else {
                        double held = 0.0;
                        if (down_time[index] > 0) {
                            held = now - down_time[index];
                        }
                        if (held < TAP_THRESHOLD) {
                            printf("button %u TAP (state %u)\n", index+1, state);
                            printf("button %u RELEASED (state %u)\n", index+1, state);
                        } else if (held >= HOLD_THRESHOLD) {
                            if (!hold_emitted[index]) {
                                printf("button %u HOLD (%.2fs)\n", index+1, held);
                            }
                            printf("button %u RELEASED (state %u)\n", index+1, state);
                        } else {
                            printf("button %u TAP (state %u)\n", index+1, state);
                            printf("button %u RELEASED (state %u)\n", index+1, state);
                        }
                        down_time[index] = 0;
                        hold_emitted[index] = 0;
                        tap_pending[index] = 0;
                    }
                    last_wake = time(NULL);
                }
            }
        } else if (res == 0) {
            // periodic wake to keep events flowing
            time_t nowt = time(NULL);
            if (nowt - last_wake >= 2) {
                wake_device(dev);
                last_wake = nowt;
            }
            // emit HOLD if still pressed with no release
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            double now = (double)ts_now.tv_sec + (double)ts_now.tv_nsec / 1e9;
            for (int i=0;i<14;i++) {
                if (down_time[i] > 0 && !hold_emitted[i] && tap_pending[i]) {
                    double held = now - down_time[i];
                    if (held >= HOLD_THRESHOLD) {
                        printf("button %u HOLD (%.2fs)\n", i+1, held);
                        hold_emitted[i] = 1;
                        tap_pending[i] = 0;
                    }
                }
            }
        } else {
            const wchar_t *err = hid_error(dev);
            fprintf(stderr, "[debug] hid_read failed: %d (%ls)\n", res, err ? err : L"?");
            break;
        }
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000; // 10ms
        nanosleep(&ts, NULL);
    }
    hid_close(dev);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];
    const char *device_path = NULL;

    hid_init();

    if (strcmp(cmd, "set-buttons") == 0) {
        const char *zip = NULL;
        for (int i=2;i<argc;i++) {
            if (strcmp(argv[i], "--zip")==0 && i+1<argc) { zip=argv[++i]; }
            else if (strncmp(argv[i],"--device-path=",14)==0) { device_path=argv[i]+14; }
            else if (strcmp(argv[i],"--device-path")==0 && i+1<argc) { device_path=argv[++i]; }
        }
        if (!zip) { usage(); return 1; }
        return cmd_set_buttons(zip, device_path);
    }
    else if (strcmp(cmd, "set-brightness") == 0) {
        if (argc < 3) { usage(); return 1; }
        int val = atoi(argv[2]);
        for (int i=3;i<argc;i++) {
            if (strncmp(argv[i],"--device-path=",14)==0) device_path=argv[i]+14;
            else if (strcmp(argv[i],"--device-path")==0 && i+1<argc) device_path=argv[++i];
        }
        return cmd_set_brightness(val, device_path);
    }
    else if (strcmp(cmd, "set-small-window") == 0) {
        int mode=1,cpu=0,mem=0,gpu=0;
        const char *time_str="00:00:00";
        for (int i=2;i<argc;i++) {
            if (strncmp(argv[i],"--mode=",7)==0) mode=atoi(argv[i]+7);
            else if (strncmp(argv[i],"--cpu=",6)==0) cpu=atoi(argv[i]+6);
            else if (strncmp(argv[i],"--mem=",6)==0) mem=atoi(argv[i]+6);
            else if (strncmp(argv[i],"--gpu=",6)==0) gpu=atoi(argv[i]+6);
            else if (strncmp(argv[i],"--time=",7)==0) time_str=argv[i]+7;
            else if (strcmp(argv[i],"--device-path")==0 && i+1<argc) device_path=argv[++i];
            else if (strncmp(argv[i],"--device-path=",14)==0) device_path=argv[i]+14;
        }
        return cmd_set_small_window(mode,cpu,mem,gpu,time_str,device_path);
    }
    else if (strcmp(cmd, "set-label-style") == 0) {
        const char *json=NULL;
        for (int i=2;i<argc;i++) {
            if (strcmp(argv[i],"--json")==0 && i+1<argc) json=argv[++i];
            else if (strncmp(argv[i],"--device-path=",14)==0) device_path=argv[i]+14;
            else if (strcmp(argv[i],"--device-path")==0 && i+1<argc) device_path=argv[++i];
        }
        if (!json) { usage(); return 1; }
        return cmd_set_label_style(json, device_path);
    }
    else if (strcmp(cmd, "ping") == 0) {
        for (int i=2;i<argc;i++) {
            if (strncmp(argv[i],"--device-path=",14)==0) device_path=argv[i]+14;
            else if (strcmp(argv[i],"--device-path")==0 && i+1<argc) device_path=argv[++i];
        }
        return cmd_ping(device_path);
    }
    else if (strcmp(cmd, "keep-alive") == 0) {
        int interval=25;
        for (int i=2;i<argc;i++) {
            if (strncmp(argv[i],"--interval=",11)==0) interval=atoi(argv[i]+11);
            else if (strcmp(argv[i],"--device-path")==0 && i+1<argc) device_path=argv[++i];
            else if (strncmp(argv[i],"--device-path=",14)==0) device_path=argv[i]+14;
        }
        return cmd_keep_alive(interval, device_path);
    }
    else if (strcmp(cmd, "read-buttons") == 0) {
        for (int i=2;i<argc;i++) {
            if (strncmp(argv[i],"--device-path=",14)==0) device_path=argv[i]+14;
            else if (strcmp(argv[i],"--device-path")==0 && i+1<argc) device_path=argv[++i];
        }
        return cmd_read_buttons(device_path);
    }

    usage();
    return 1;
}

// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "font8x8.h"

#define SCREEN_WIDTH  1280
#define SCREEN_HEIGHT 720

#define COLOR_BLACK   0xFF000000
#define COLOR_WHITE   0xFFFFFFFF
#define COLOR_YELLOW  0xFFFFD700
#define COLOR_CYAN    0xFF00E5FF
#define COLOR_GRAY    0xFFAAAAAA

#define PAD_BUTTON_OPTIONS 0x00000008
#define PAD_BUTTON_CROSS   0x00004000

typedef struct {
    uint32_t reserved0;
    uint32_t tiling_mode;
    uint32_t aspect_ratio;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_in_pixel;
    uint64_t option;
    uint64_t pixel_format;
    uint64_t dcc_cb_register_clear_color;
    uint32_t dcc_control;
    uint32_t pad0;
    uint64_t reserved1[3];
} VideoOutBufferAttribute2;

typedef struct {
    const void* data;
    const void* metadata;
    const void* reserved[2];
} VideoOutBuffers;

typedef struct {
    uint32_t buttons;
    uint8_t  left_stick_x;
    uint8_t  left_stick_y;
    uint8_t  right_stick_x;
    uint8_t  right_stick_y;
    uint8_t  analog_buttons_l2;
    uint8_t  analog_buttons_r2;
    uint8_t  padding[2];
    float    orientation[4];
    float    acceleration[3];
    float    angular_velocity[3];
    uint8_t  touch_data[24];
    uint8_t  connected;
    uint64_t timestamp;
    uint8_t  reserved[32];
} PadData;

// PS5 APIs
extern int sceVideoOutOpen(int user_id, int bus_type, int index, const void* param);
extern void sceVideoOutSetBufferAttribute2(VideoOutBufferAttribute2* attribute,
                                           uint64_t pixel_format, uint32_t tiling_mode,
                                           uint32_t width, uint32_t height, uint64_t option,
                                           uint32_t dcc_control, uint64_t dcc_cb_register_clear_color);
extern int sceVideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start,
                                       const VideoOutBuffers* buffers, int buffer_num,
                                       const VideoOutBufferAttribute2* attribute, int category,
                                       void* option);
extern int sceVideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg);

extern int scePadOpen(int user_id, int type, int index, const void* param);
extern int scePadReadState(int handle, PadData* data);
extern int sceKernelUsleep(uint32_t microseconds);

// 64KB aligned framebuffers
static uint32_t s_framebuffers[2][SCREEN_WIDTH * SCREEN_HEIGHT] __attribute__((aligned(65536)));

static void draw_pixel(uint32_t* fb, int x, int y, uint32_t color) {
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        fb[y * SCREEN_WIDTH + x] = color;
    }
}

static void draw_char_scaled(uint32_t* fb, int x, int y, char c, uint32_t color, int scale) {
    if ((unsigned char)c >= 128) return;
    const uint8_t* glyph = (const uint8_t*)font8x8_basic[(unsigned char)c];
    for (int cy = 0; cy < 8; cy++) {
        uint8_t row = glyph[cy];
        for (int cx = 0; cx < 8; cx++) {
            if (row & (1 << cx)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        draw_pixel(fb, x + cx * scale + sx, y + cy * scale + sy, color);
                    }
                }
            }
        }
    }
}

static void draw_text(uint32_t* fb, int x, int y, const char* str, uint32_t color, int scale) {
    int cur_x = x;
    while (*str) {
        draw_char_scaled(fb, cur_x, y, *str, color, scale);
        cur_x += 8 * scale;
        str++;
    }
}

static void int_to_str(uint32_t val, char* buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char temp[16];
    int len = 0;
    while (val > 0) {
        temp[len++] = '0' + (val % 10);
        val /= 10;
    }
    for (int i = 0; i < len; i++) {
        buf[i] = temp[len - 1 - i];
    }
    buf[len] = '\0';
}

int main(int argc, char* argv[]) {
    // 1. Initialize VideoOut
    int video_handle = sceVideoOutOpen(0xFF, 0, 0, NULL);
    if (video_handle <= 0) {
        video_handle = 1; // Fallback handle
    }

    VideoOutBufferAttribute2 attr = {0};
    // Format: 0x8000000022000000ull (R8G8B8A8 sRGB)
    sceVideoOutSetBufferAttribute2(&attr, 0x8000000022000000ULL, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0);

    VideoOutBuffers vbufs[2] = {
        { .data = s_framebuffers[0], .metadata = NULL },
        { .data = s_framebuffers[1], .metadata = NULL }
    };
    sceVideoOutRegisterBuffers2(video_handle, 0, 0, vbufs, 2, &attr, 0, NULL);

    // 2. Initialize Pad
    int pad_handle = scePadOpen(0xFF, 0, 0, NULL);
    if (pad_handle <= 0) {
        pad_handle = 1;
    }

    uint32_t frame_counter = 0;
    uint32_t cross_presses = 0;
    bool last_cross_down   = false;
    int current_buffer     = 0;

    // Simulation loop
    while (true) {
        uint32_t* fb = s_framebuffers[current_buffer];

        // Clear screen to black
        for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
            fb[i] = COLOR_BLACK;
        }

        // Read Pad input
        PadData pad_data = {0};
        scePadReadState(pad_handle, &pad_data);

        bool cross_down = (pad_data.buttons & PAD_BUTTON_CROSS) != 0;
        if (cross_down && !last_cross_down) {
            cross_presses++;
        }
        last_cross_down = cross_down;

        if (pad_data.buttons & PAD_BUTTON_OPTIONS) {
            break;
        }

        frame_counter++;

        // Render UI
        draw_text(fb, 24, 24, "FPS: 60.0", COLOR_YELLOW, 2);
        draw_text(fb, 520, 24, "CapyPS5 | 0.1.0 | Odin3 | Adreno 830 | CQ8725S", COLOR_YELLOW, 2);

        draw_text(fb, 24, 80, "CapyPS5 test homebrew", COLOR_WHITE, 3);

        char frame_str[64] = "Frame counter:        ";
        char frame_num[16];
        int_to_str(frame_counter, frame_num);
        for (int i = 0; frame_num[i] != '\0'; i++) {
            frame_str[22 + i] = frame_num[i];
            frame_str[22 + i + 1] = '\0';
        }
        draw_text(fb, 24, 150, frame_str, COLOR_WHITE, 2);

        char cross_str[64] = "Cross-button presses: ";
        char cross_num[16];
        int_to_str(cross_presses, cross_num);
        for (int i = 0; cross_num[i] != '\0'; i++) {
            cross_str[22 + i] = cross_num[i];
            cross_str[22 + i + 1] = '\0';
        }
        draw_text(fb, 24, 190, cross_str, COLOR_WHITE, 2);

        draw_text(fb, 24, 260, "Press Cross (X/A) to increment, Options (+) to exit.", COLOR_GRAY, 2);

        // Submit flip
        sceVideoOutSubmitFlip(video_handle, current_buffer, 1, 0);
        current_buffer ^= 1;

        // 60 FPS pacing (~16.6ms)
        sceKernelUsleep(16666);
    }

    return 0;
}

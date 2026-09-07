/*
 * render.cpp - turn Gyruss's 256x224 native frame upright on the 240x280 panel.
 *
 * The cabinet's monitor is rotated, so the native frame is the picture lying on its side. The
 * upright picture is 224 wide by 256 tall, which stretches to fill the whole panel almost
 * exactly (1.071 across, 1.094 down), leaving no bars.
 *
 * The rotation is folded into the row walk: a panel row is a fixed native column, so the
 * source pointer steps by one native row per panel pixel.
 */
#include "render.h"
#include "gyruss.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "RENDER";
#define ROWS_PER_CHUNK 14
#define NUM_FB 2
#define ROT_W GY_FB_H          /* 224: the upright picture's width  */
#define ROT_H GY_FB_W          /* 256: and its height               */

static uint8_t *fbs[NUM_FB];
static QueueHandle_t free_q, frame_q;
static uint16_t *chunk;
static uint16_t pal_swapped[GY_PALETTE_SIZE];
static uint16_t x_map[DISPLAY_WIDTH];      /* panel column -> upright column */
static uint16_t y_map[DISPLAY_HEIGHT];     /* panel row    -> upright row    */
static uint32_t frames_drawn, frames_dropped;
static uint64_t busy_us;

static void present(const uint8_t *fb)
{
    uint16_t pal[GY_PALETTE_SIZE];
    gy_palette(pal);
    for (int i = 0; i < GY_PALETTE_SIZE; i++) pal_swapped[i] = (uint16_t)((pal[i] >> 8) | (pal[i] << 8));
    display_set_window(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (int row = 0; row < DISPLAY_HEIGHT; row += ROWS_PER_CHUNK) {
        int rows = (row + ROWS_PER_CHUNK <= DISPLAY_HEIGHT) ? ROWS_PER_CHUNK : (DISPLAY_HEIGHT - row);
        uint16_t *dst = chunk;
        for (int r = 0; r < rows; r++) {
            /* upright (rx, ry) is native (x = ry, y = 223 - rx) */
            int ry = y_map[row + r];
            const uint8_t *src = fb + ry;             /* native column ry */
            for (int px = 0; px < DISPLAY_WIDTH; px++) {
                int rx = x_map[px];
                *dst++ = pal_swapped[src[(GY_FB_H - 1 - rx) * GY_FB_W]];
            }
        }
        display_write_preswapped(chunk, rows * DISPLAY_WIDTH);
    }
    display_wait_done();
}

static void render_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t *fb;
        if (xQueueReceive(frame_q, &fb, portMAX_DELAY) != pdTRUE) continue;
        int64_t t0 = esp_timer_get_time();
        present(fb);
        busy_us += esp_timer_get_time() - t0;
        xQueueSend(free_q, &fb, 0);
        frames_drawn++;
    }
}

void render_init(void)
{
    for (int i = 0; i < DISPLAY_WIDTH; i++)  x_map[i] = (uint16_t)(i * ROT_W / DISPLAY_WIDTH);
    for (int i = 0; i < DISPLAY_HEIGHT; i++) y_map[i] = (uint16_t)(i * ROT_H / DISPLAY_HEIGHT);
    chunk = (uint16_t *)heap_caps_malloc(ROWS_PER_CHUNK * DISPLAY_WIDTH * sizeof(uint16_t), MALLOC_CAP_8BIT);
    free_q = xQueueCreate(NUM_FB, sizeof(uint8_t *));
    frame_q = xQueueCreate(NUM_FB, sizeof(uint8_t *));
    for (int i = 0; i < NUM_FB; i++) {
        fbs[i] = (uint8_t *)heap_caps_malloc(GY_FB_W * GY_FB_H, MALLOC_CAP_8BIT);
        if (!fbs[i]) { ESP_LOGE(TAG, "frame buffer allocation failed"); abort(); }
        xQueueSend(free_q, &fbs[i], 0);
    }
    if (!chunk) { ESP_LOGE(TAG, "chunk allocation failed"); abort(); }
    xTaskCreate(render_task, "render", 4096, nullptr, 6, nullptr);
    ESP_LOGI(TAG, "render task started (%dx%d native -> %dx%d upright, full panel)",
             GY_FB_W, GY_FB_H, ROT_W, ROT_H);
}

uint8_t *render_acquire(void)
{
    uint8_t *fb;
    if (xQueueReceive(free_q, &fb, 0) != pdTRUE) { frames_dropped++; return nullptr; }
    return fb;
}
void render_submit(uint8_t *fb) { xQueueSend(frame_q, &fb, 0); }
uint32_t render_frames_drawn(void) { uint32_t v = frames_drawn; frames_drawn = 0; return v; }
uint32_t render_frames_dropped(void) { uint32_t v = frames_dropped; frames_dropped = 0; return v; }
uint64_t render_busy_us(void) { uint64_t v = busy_us; busy_us = 0; return v; }

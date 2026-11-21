// Doom entry point for the AI Passport: runs D_DoomMain on its own task,
// draws via the board display, maps the 3 ADC buttons to Doom keys.
#include <cstdint>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include "doomdef.h"
#include "d_main.h"
#include "display/display.h"
#include "display/lcd_display.h"
// DisplayLockGuard comes from display.h

#define TAG "Doom"

extern "C" {
void DoomPushKey(int keyd, bool down);
void I_FreeVideoBuffers(void);   // i_video_esp32.c
void wad_pack_shutdown(void);    // wad_pack.c
}

static TaskHandle_t s_task = nullptr;
static volatile bool s_running = false;
static volatile bool s_stop_requested = false;
static Display* s_display = nullptr;
static LcdDisplay* s_lcd = nullptr;
static uint16_t* s_frame = nullptr;  // 240x320 RGB565 DMA staging buffer

// Button -> Doom key mapping (KEYD_* from doomdef.h).
// up = forward, down = back, ok = fire, ok-double = use, ok-hold = menu(esc),
// hold up = turn left, hold down = turn right.
void DoomOnButton(int button, int gesture);  // button: 0=up 1=down 2=ok; gesture: 0=click 1=double 2=hold

extern "C" void DoomFatalError(const char* msg) {
    ESP_LOGE(TAG, "fatal: %s", msg);
    s_running = false;
    s_stop_requested = true;
}

extern "C" void DoomRequestStop() { s_stop_requested = true; }

// Draw one frame: upscale 120x160 indexed -> 240x320 RGB565 and push.
extern "C" void DoomDrawFrame(const uint8_t* indexed, const uint16_t* pal565, int w, int h) {
    if (s_lcd == nullptr) {
        return;
    }
    if (s_frame == nullptr) {
        s_frame = (uint16_t*)heap_caps_malloc(240 * 320 * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    if (s_frame == nullptr) {
        return;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t* src = indexed + y * w;
        uint16_t* dst0 = s_frame + (y * 2) * 240;
        uint16_t* dst1 = s_frame + (y * 2 + 1) * 240;
        for (int x = 0; x < w; x++) {
            uint16_t c = pal565[src[x]];
            dst0[x * 2] = c;
            dst0[x * 2 + 1] = c;
            dst1[x * 2] = c;
            dst1[x * 2 + 1] = c;
        }
    }
    // Hold the LVGL lock while we own the panel (DisplayLockGuard is public).
    {
        DisplayLockGuard lock(s_display);
        s_lcd->DrawPixels(0, 0, 240, 320, s_frame);
    }
}

// Free every heap buffer Doom owns (called from the engine's I_Quit path).
extern "C" void DoomShutdownCleanup(void) {
    s_running = false;
    s_stop_requested = true;
    if (s_frame != nullptr) {
        heap_caps_free(s_frame);
        s_frame = nullptr;
    }
    I_FreeVideoBuffers();
    wad_pack_shutdown();
    ESP_LOGI(TAG, "doom buffers freed, heap=%u", (unsigned)esp_get_free_heap_size());
}

static void DoomTask(void*) {
    ESP_LOGI(TAG, "Doom task start, heap=%u", (unsigned)esp_get_free_heap_size());
    // D_DoomMain never returns; it calls I_Error -> DoomFatalError on failure.
    D_DoomMain();
    s_running = false;
    DoomShutdownCleanup();
    vTaskDelete(nullptr);
}

bool DoomIsRunning() { return s_running; }

void DoomStart(Display* display) {
    if (s_running) {
        return;
    }
    s_display = display;
    s_lcd = static_cast<LcdDisplay*>(display);  // the passport board uses LcdDisplay
    s_stop_requested = false;
    s_running = true;
    // Heap-allocated stack: the C3 has no RAM to spare at boot while music/MQTT
    // is idle, so the stack must only exist while Doom actually runs.
    BaseType_t rc = xTaskCreate(DoomTask, "doom", 12288, nullptr, 3, &s_task);
    if (rc != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "failed to create doom task");
    }
}

void DoomStop() {
    s_stop_requested = true;
    DoomPushKey(KEYD_START, true);  // escape menu if open
    DoomPushKey(KEYD_START, false);
}

// Called by the board's button handlers while Doom runs.
// button: 0=up, 1=down, 2=ok; gesture: 0=click, 1=double, 2=hold
void DoomOnButton(int button, int gesture) {
    if (!s_running) {
        return;
    }
    auto press = [](int key) { DoomPushKey(key, true); DoomPushKey(key, false); };
    switch (button) {
        case 0:  // up
            if (gesture == 2) { DoomPushKey(KEYD_LEFT, true); }       // hold = turn left (held)
            else { press(KEYD_UP); }
            break;
        case 1:  // down
            if (gesture == 2) { DoomPushKey(KEYD_RIGHT, true); }      // hold = turn right
            else { press(KEYD_DOWN); }
            break;
        case 2:  // ok
            if (gesture == 2) { press(KEYD_START); }                  // hold = menu/esc
            else if (gesture == 1) { press(KEYD_B); }                 // double = use (KEYD_B acts as use in GBA map)
            else { press(KEYD_A); }                                   // click = fire
            break;
    }
}

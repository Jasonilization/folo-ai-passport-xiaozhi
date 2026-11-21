// ESP32-C3 platform layer for prBoom: timing, error handling, key input.
// NOTE: engine headers lack extern "C" guards, so we declare the platform
// interface manually inside extern "C" instead of including them.
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <esp_log.h>

#define TAG "DoomSys"

extern "C" {

// Engine-facing interface (C linkage, called from the .c engine files).
// d_event.h layout, replicated here to avoid the non-C++-safe engine header.
extern "C" {
typedef enum { ev_keydown = 0, ev_keyup = 1 } evtype_t;
typedef struct { evtype_t type; int data1, data2, data3; } event_t;
void D_PostEvent(event_t* ev);
void DoomShutdownCleanup(void);
}
typedef event_t event_t_;

void DoomPushKey(int keyd, bool down);
void DoomFatalError(const char* msg);
void DoomRequestStop();

static QueueHandle_t s_key_queue = nullptr;

void DoomPushKey(int keyd, bool down) {
    if (s_key_queue == nullptr) return;
    event_t_ ev;
    ev.type = down ? ev_keydown : ev_keyup;
    ev.data1 = keyd;
    ev.data2 = ev.data3 = 0;
    xQueueSendFromISR(s_key_queue, &ev, nullptr);
}

void I_Sleep(int ms) { (void)ms; }
void I_Error(const char* error, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, error);
    vsnprintf(buf, sizeof(buf), error, ap);
    va_end(ap);
    ESP_LOGE(TAG, "I_Error: %s", buf);
    DoomFatalError(buf);
    vTaskSuspend(nullptr);
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
void I_Quit(void) {
    // The engine calls this from its menu "quit" path. Free the buffers we own
    // and end this task - D_DoomMain never returns after I_Quit.
    DoomShutdownCleanup();
    vTaskDelete(nullptr);
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
bool I_StartDisplay(void) { return true; }
void I_EndDisplay(void) {}
const char* I_GetVersionString(void) { return "ESP32-C3 Doom"; }
void I_Init(void) {
    if (s_key_queue == nullptr) s_key_queue = xQueueCreate(32, sizeof(event_t_));
}
void I_AtExit(void) {}
void I_ProcessKeyEvents(void) {
    if (s_key_queue == nullptr) return;
    event_t ev;
    while (xQueueReceive(s_key_queue, &ev, 0) == pdTRUE) {
        D_PostEvent(&ev);
    }
}

}  // extern "C"

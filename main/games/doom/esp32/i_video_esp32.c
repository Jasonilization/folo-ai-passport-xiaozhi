// ESP32-C3 video layer: 120x160 indexed backbuffer upscaled x2 to the
// 240x320 ST7789 panel via the display's DrawPixels API.
#include <stdint.h>
#include <string.h>

#include "doomdef.h"
#include "doomtype.h"
#include "v_video.h"
#include "i_video.h"
#include "global_data.h"
#include "w_wad.h"
#include "lprintf.h"

// Display handle injected by doom_entry.cpp
extern void* g_doom_display;  // Display*
// Draw into the LCD: implemented in doom_entry.cpp via the Display API.
extern void DoomDrawFrame(const uint8_t* indexed, const uint16_t* pal565, int w, int h);

static uint8_t s_backbuffer[SCREENWIDTH * SCREENHEIGHT];
static uint16_t s_palette[256];

void I_PreInitGraphics(void) {}

void I_SetRes(void) {
    _g->screens[0].width = SCREENWIDTH;
    _g->screens[0].height = SCREENHEIGHT;
    _g->screens[0].data = ((unsigned short*)s_backbuffer);
}

void I_InitGraphics(void) {
    I_SetRes();
    lprintf("I_InitGraphics: %dx%d", SCREENWIDTH, SCREENHEIGHT);
}

void I_SetPalette(int pal) {
    // Convert the standard PLAYPAL lump (768 bytes RGB888) to RGB565.
    // pal selects the gamma palette (0..13); we ignore gamma on the LCD.
    (void)pal;
    const byte* playpal = (const byte*)W_CacheLumpName("PLAYPAL");
    if (playpal == NULL) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        uint8_t r = playpal[i * 3 + 0];
        uint8_t g = playpal[i * 3 + 1];
        uint8_t b = playpal[i * 3 + 2];
        s_palette[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    _g->current_pallete = (const unsigned short*)s_palette;
}

void I_UpdateNoBlit(void) {}

void I_FinishUpdate(void) {
    DoomDrawFrame(s_backbuffer, s_palette, SCREENWIDTH, SCREENHEIGHT);
}

void I_StartTic(void) {
    extern void I_ProcessKeyEvents(void);
    I_ProcessKeyEvents();
}

void I_StartFrame(void) {}

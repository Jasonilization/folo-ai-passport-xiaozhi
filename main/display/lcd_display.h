#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* activation_qrcode_ = nullptr;
    lv_obj_t* activation_hint_label_ = nullptr;
    lv_obj_t* activation_site_label_ = nullptr;
    lv_obj_t* activation_code_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    // Minimal, icon-only mood: a single Material Symbols glyph plus a cheap
    // breathe/bob animation. Replaces the emoji image path entirely.
    const char* mood_glyph_ = nullptr;
    uint32_t mood_period_ms_ = 0;  // 0 = static, no mood timer
    bool mood_bob_ = false;
    int mood_phase_ = 0;
    lv_timer_t* mood_timer_ = nullptr;

    static void MoodTimerCb(lv_timer_t* timer);

    // Expressive cylinder-eyes avatar (replaces the glyph mood icon). Off by
    // default; toggled with the "eyes" console command / NVS key "eyes".
    bool eyes_enabled_ = false;   // user preference (survives reboot)
    bool eyes_read_nvs_ = false;  // preference loaded yet
    bool eyes_ready_ = false;     // objects created and visible
    lv_obj_t* eyes_cont_ = nullptr;
    lv_obj_t* eye_l_ = nullptr;
    lv_obj_t* eye_r_ = nullptr;
    lv_obj_t* pupil_l_ = nullptr;
    lv_obj_t* pupil_r_ = nullptr;
    lv_obj_t* glint_l_ = nullptr;
    lv_obj_t* glint_r_ = nullptr;
    lv_timer_t* eyes_timer_ = nullptr;
    int eyes_kind_ = 0;           // 0 idle, 1 thinking, 2 listening, 3 speaking, 4 sleepy
    int eyes_ticks_ = 0;
    int eyes_blink_ = 0;          // >0: frames left in the blink animation
    int eyes_gaze_timer_ = 0;
    int eyes_gaze_x_ = 0;         // current gaze offset (px, -12..12)
    int eyes_gaze_y_ = 0;
    int eyes_gaze_tx_ = 0;        // gaze target
    int eyes_gaze_ty_ = 0;
    int eyes_high_ = 0;           // glint/pupil highlight phase
    void CreateEyes();
    void DestroyEyes();
    void UpdateEyesKind(const char* mood);
    void DrawEyesFrame();
    static void EyesTimerCb(lv_timer_t* timer);

    // Microphone level meter: a small bar row at the bottom edge, driven by
    // the smoothed RMS from AudioService. Attack is fast, decay is slow.
    static constexpr int kMicVizBars = 16;
    static constexpr int kMicVizBarHeight = 14;  // full-scale bar height (px)
    static constexpr int kMicVizBarWidth = 5;    // bar width (px)
    static constexpr int kMicVizBarGap = 2;      // gap between bars (px)
    lv_obj_t* mic_viz_bar_[kMicVizBars] = {};
    lv_timer_t* mic_viz_timer_ = nullptr;
    bool mic_viz_enabled_ = true;
    float mic_level_target_ = 0.0f;  // latest level pushed from AudioService
    float mic_level_shown_ = 0.0f;   // animated level actually drawn
    void CreateMicViz();
    void UpdateMicViz();
    static void MicVizTimerCb(lv_timer_t* timer);

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ShowActivationCode(const char* code, const char* message) override;
    virtual void ClearActivationCode() override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    virtual void SetHideSubtitle(bool hide) override;

    // Scroll the chat history by a vertical step (history scroll-back)
    virtual void ScrollChatMessages(int scroll_step_y) override;

    // Live microphone loudness (0..1) for the level meter
    virtual void SetMicLevel(float level) override;
    // Mood face: glyph icon or (when enabled) the animated cylinder eyes.
    virtual void UpdateMood(const char* mood);

    // Toggle the animated cylinder-eyes avatar (persisted in NVS).
    void SetEyesEnabled(bool enabled);
    bool EyesEnabled() const { return eyes_enabled_; }
    // Raw panel blit for full-screen apps (Doom); caller holds the lock.
    void DrawPixels(int x, int y, int w, int h, const uint16_t* rgb565) override;
    virtual void SetMicVisualizerEnabled(bool enabled) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H

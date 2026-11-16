#ifndef MAIN_MENU_H
#define MAIN_MENU_H

#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <material_symbols.h>

#include <lvgl.h>
#include <functional>
#include <string>
#include <vector>

// Full-screen main menu overlay for the AI Passport.
//
// English-only by design (the whole UI is English-only on this build), so the
// strings are literal instead of going through Lang::Strings.
//
// Gesture model (board wires the buttons):
//   home:    OK hold            -> open menu
//   menu:    up/down            -> move selection
//            OK click           -> activate selection
//            OK hold            -> close menu
class MainMenu {
public:
    struct Item {
        int id = 0;
        std::string label;
        const char* icon = nullptr;  // Material Symbols glyph
    };

    explicit MainMenu(LcdDisplay* display) : display_(display) {}

    using ActivateCb = std::function<void(int id)>;
    void SetActivateCallback(ActivateCb cb) { on_activate_ = std::move(cb); }

    bool IsOpen() const { return open_; }
    void Open(std::vector<Item> items);
    void Close();
    void MoveSelection(int delta);  // -1 = up, +1 = down
    // Held up/down repeats the movement for fast scrolling.
    void MoveHoldStart(int delta);
    void HoldStop();
    void Activate();

private:
    void Build();
    void UpdateSelection();
    static void OnTimer(lv_timer_t* timer);
    static void OnHoldTimer(lv_timer_t* timer);
    // Per-frame animation: slides the selection cursor and pulses the icons.
    void Animate();

    LcdDisplay* display_ = nullptr;
    bool open_ = false;
    int selected_ = 0;
    std::vector<Item> items_;
    ActivateCb on_activate_;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* cursor_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    lv_timer_t* hold_timer_ = nullptr;
    int hold_delta_ = 0;
    int cursor_y_ = 0;    // current cursor position (animated)
    int cursor_target_ = 0;
    bool cursor_snap_ = true;  // snap (don't slide) on the next selection
    uint32_t tick_ = 0;
    std::vector<lv_obj_t*> row_;
    std::vector<lv_obj_t*> row_icon_;
    std::vector<lv_obj_t*> row_text_;
};

#endif
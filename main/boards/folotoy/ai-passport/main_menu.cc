#include "main_menu.h"

#include <esp_log.h>

#define TAG "MainMenu"

namespace {
LvglTheme* ThemeOf(LcdDisplay* display) {
    return static_cast<LvglTheme*>(display->GetTheme());
}
// Accent used for the selection highlight (dark theme -> vivid amber).
constexpr uint32_t kAccent = 0xFFB020;
// Per-item icon hues so the menu reads as a colorful launcher, not a list.
lv_color_t IconHue(int id) {
    static const uint32_t kPalette[] = {
        0xFFB020, 0x4FC3F7, 0x81C784, 0xFF8A65, 0xBA68C8, 0xFFF176, 0x4DB6AC, 0xF06292,
    };
    return lv_color_hex(kPalette[((id % 8) + 8) % 8]);
}
}  // namespace

void MainMenu::Open(std::vector<Item> items) {
    if (display_ == nullptr) {
        return;
    }
    items_ = std::move(items);
    selected_ = 0;
    open_ = true;
    cursor_snap_ = true;
    if (timer_ != nullptr) {
        lv_timer_resume(timer_);
    }

    DisplayLockGuard lock(display_);
    if (panel_ == nullptr) {
        Build();
        if (panel_ == nullptr) {
            return;
        }
    } else {
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    }

    // (Re)build rows: the item set can change between opens. Note that
    // lv_obj_clean(list_) also deletes the cursor child, so drop our pointer
    // and recreate it below.
    lv_obj_clean(list_);
    cursor_ = nullptr;
    row_.clear();
    row_icon_.clear();
    row_text_.clear();

    auto* theme = ThemeOf(display_);
    if (theme == nullptr) {
        return;
    }
    for (const auto& item : items_) {
        auto* r = lv_obj_create(list_);
        lv_obj_set_size(r, LV_PCT(100), 34);
        lv_obj_set_style_radius(r, 0, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_pad_left(r, 12, 0);
        lv_obj_set_style_pad_right(r, 12, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 10, 0);

        auto* ic = lv_label_create(r);
        lv_obj_set_style_text_font(ic, theme->icon_font()->font(), 0);
        lv_obj_set_style_text_color(ic, IconHue(item.id), 0);
        lv_label_set_text(ic, item.icon != nullptr ? item.icon : "");

        auto* tx = lv_label_create(r);
        lv_obj_set_style_text_font(tx, theme->text_font()->font(), 0);
        lv_label_set_text(tx, item.label.c_str());

        row_.push_back(r);
        row_icon_.push_back(ic);
        row_text_.push_back(tx);
    }
    UpdateSelection();
}

void MainMenu::Close() {
    open_ = false;
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
    if (panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void MainMenu::Build() {
    auto* theme = ThemeOf(display_);
    if (theme == nullptr) {
        return;
    }

    auto* screen = lv_screen_active();
    int w = lv_obj_get_width(screen);
    int h = lv_obj_get_height(screen);

    panel_ = lv_obj_create(screen);
    lv_obj_set_size(panel_, w - 12, h - 12);
    lv_obj_center(panel_);
    lv_obj_set_style_radius(panel_, 0, 0);
    lv_obj_set_style_bg_color(panel_, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel_, theme->border_color(), 0);
    lv_obj_set_style_border_width(panel_, 1, 0);
    lv_obj_set_style_pad_all(panel_, 8, 0);
    lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);

    title_label_ = lv_label_create(panel_);
    lv_obj_set_style_text_font(title_label_, theme->large_icon_font()->font(), 0);
    lv_obj_set_style_text_color(title_label_, theme->text_color(), 0);
    lv_label_set_text(title_label_, "MENU");
    lv_obj_align(title_label_, LV_ALIGN_TOP_LEFT, 4, 2);

    hint_label_ = lv_label_create(panel_);
    lv_obj_set_style_text_font(hint_label_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(hint_label_, theme->text_color(), 0);
    lv_obj_set_style_text_opa(hint_label_, LV_OPA_60, 0);
    lv_label_set_text(hint_label_, "OK select  |  HOLD close");
    lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    list_ = lv_obj_create(panel_);
    lv_obj_set_size(list_, LV_PCT(100), h - 12 - 60);
    lv_obj_align(list_, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_, 0, 0);
    lv_obj_set_style_pad_all(list_, 0, 0);
    lv_obj_set_style_pad_row(list_, 2, 0);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Animated selection cursor: slides between rows and keeps the icons
    // breathing so the menu reads as alive rather than a static list.
    cursor_ = lv_obj_create(list_);
    lv_obj_set_size(cursor_, 5, 34);
    lv_obj_set_x(cursor_, 1);
    lv_obj_set_style_radius(cursor_, 0, 0);
    lv_obj_set_style_bg_color(cursor_, lv_color_hex(kAccent), 0);
    lv_obj_set_style_bg_opa(cursor_, LV_OPA_70, 0);
    lv_obj_set_style_border_width(cursor_, 0, 0);
    lv_obj_set_style_pad_all(cursor_, 0, 0);
    lv_obj_clear_flag(cursor_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(cursor_);
    lv_obj_set_y(cursor_, 0);
    cursor_y_ = 0;
    cursor_target_ = 0;
    if (timer_ == nullptr) {
        timer_ = lv_timer_create(OnTimer, 30, this);
    }
}

void MainMenu::OnTimer(lv_timer_t* timer) {
    auto* self = static_cast<MainMenu*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->Animate();
    }
}

void MainMenu::Animate() {
    if (cursor_ == nullptr || !lv_obj_is_valid(cursor_) || row_.empty() || !open_) {
        return;
    }
    // Ease the cursor toward its target row (40% per frame).
    if (cursor_y_ != cursor_target_) {
        int delta = cursor_target_ - cursor_y_;
        cursor_y_ += (delta * 4) / 10;
        if ((delta > 0 && cursor_y_ > cursor_target_) ||
            (delta < 0 && cursor_y_ < cursor_target_)) {
            cursor_y_ = cursor_target_;
        }
        DisplayLockGuard lock(display_);
        lv_obj_set_y(cursor_, cursor_y_);
    }
    // Gentle pulse on the selected row's icon (triangle wave, no trig table).
    if (selected_ >= 0 && selected_ < static_cast<int>(row_icon_.size()) &&
        row_icon_[selected_] != nullptr) {
        int phase = tick_ % 40;
        int amp = phase < 20 ? phase : 40 - phase;
        lv_obj_set_style_text_opa(row_icon_[selected_],
                                  (lv_opa_t)(255 - amp * 2), 0);
    }
    tick_++;
}

void MainMenu::UpdateSelection() {
    if (row_.empty()) {
        return;
    }
    // Reset icon opacity so the previous selection stops breathing.
    for (auto* icon : row_icon_) {
        if (icon != nullptr) {
            lv_obj_set_style_text_opa(icon, LV_OPA_COVER, 0);
        }
    }
    if (selected_ < 0) {
        selected_ = static_cast<int>(row_.size()) - 1;
    } else if (selected_ >= static_cast<int>(row_.size())) {
        selected_ = 0;
    }

    auto* theme = ThemeOf(display_);
    if (theme == nullptr) {
        return;
    }
    for (size_t i = 0; i < row_.size(); i++) {
        bool sel = static_cast<int>(i) == selected_;
        lv_obj_set_style_bg_opa(row_[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (sel) {
            lv_obj_set_style_bg_color(row_[i], lv_color_hex(kAccent), 0);
            lv_obj_set_style_text_color(row_icon_[i], lv_color_hex(0x14100A), 0);
            lv_obj_set_style_text_color(row_text_[i], lv_color_hex(0x14100A), 0);
        } else {
            // unselected icons keep their own hue from the palette
            lv_obj_set_style_text_color(row_icon_[i], IconHue(items_[i].id), 0);
            lv_obj_set_style_text_color(row_text_[i], theme->text_color(), 0);
        }
    }
    lv_obj_scroll_to_view(row_[selected_], LV_ANIM_ON);
    // Slide the animated cursor toward the newly highlighted row.
    if (cursor_ == nullptr || !lv_obj_is_valid(cursor_)) {
        auto* theme2 = ThemeOf(display_);
        cursor_ = lv_obj_create(list_);
        lv_obj_set_size(cursor_, 5, 34);
        lv_obj_set_x(cursor_, 1);
        lv_obj_set_style_radius(cursor_, 0, 0);
        lv_obj_set_style_bg_color(cursor_, lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(cursor_, LV_OPA_70, 0);
        lv_obj_set_style_border_width(cursor_, 0, 0);
        lv_obj_set_style_pad_all(cursor_, 0, 0);
        lv_obj_clear_flag(cursor_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_background(cursor_);
        cursor_snap_ = true;
    }
    cursor_target_ = lv_obj_get_y(row_[selected_]);
    if (cursor_snap_) {
        // First selection after a rebuild: snap instead of sliding in.
        cursor_snap_ = false;
        cursor_y_ = cursor_target_;
        lv_obj_set_y(cursor_, cursor_y_);
    }
}

void MainMenu::MoveSelection(int delta) {
    if (!open_ || row_.empty()) {
        return;
    }
    selected_ += delta;
    DisplayLockGuard lock(display_);
    UpdateSelection();
}

void MainMenu::MoveHoldStart(int delta) {
    MoveSelection(delta);
    hold_delta_ = delta;
    if (hold_timer_ == nullptr) {
        hold_timer_ = lv_timer_create(OnHoldTimer, 70, this);
    } else {
        lv_timer_resume(hold_timer_);
    }
}

void MainMenu::HoldStop() {
    hold_delta_ = 0;
    if (hold_timer_ != nullptr) {
        lv_timer_pause(hold_timer_);
    }
}

void MainMenu::OnHoldTimer(lv_timer_t* timer) {
    auto* self = static_cast<MainMenu*>(lv_timer_get_user_data(timer));
    if (self != nullptr && self->hold_delta_ != 0 && self->open_) {
        self->MoveSelection(self->hold_delta_);
    }
}

void MainMenu::Activate() {
    if (!open_ || row_.empty()) {
        return;
    }
    int id = items_[selected_].id;
    ESP_LOGI(TAG, "Activated menu item %d", id);
    if (on_activate_) {
        on_activate_(id);
    }
}
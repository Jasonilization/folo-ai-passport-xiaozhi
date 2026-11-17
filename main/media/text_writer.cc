#include "text_writer.h"

#include <esp_log.h>

#include <material_symbols.h>

#define TAG "TextWriter"

namespace {
LvglTheme* ThemeOf(LcdDisplay* display) {
    return static_cast<LvglTheme*>(display->GetTheme());
}
}  // namespace

void TextWriter::Open(const std::string& initial) {
    if (display_ == nullptr) {
        return;
    }
    text_ = initial;
    selected_ = 0;
    open_ = true;

    if (cells_.empty()) {
        for (char c = 'a'; c <= 'z'; c++) {
            cells_.push_back({std::string(1, c), 0});
        }
        for (char c = '0'; c <= '9'; c++) {
            cells_.push_back({std::string(1, c), 0});
        }
        cells_.push_back({" ", 0});
        cells_.push_back({".", 0});
        cells_.push_back({"-", 0});
        cells_.push_back({"'", 0});
        cells_.push_back({"DEL", 1});
        cells_.push_back({"GO", 2});
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
    UpdateText();
    UpdateSelection();
}

void TextWriter::Close() {
    open_ = false;
    if (panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void TextWriter::Build() {
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

    text_label_ = lv_label_create(panel_);
    lv_obj_set_style_text_font(text_label_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(text_label_, theme->text_color(), 0);
    lv_label_set_text(text_label_, "");
    lv_label_set_long_mode(text_label_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(text_label_, LV_PCT(100));
    lv_obj_set_height(text_label_, 32);
    lv_obj_align(text_label_, LV_ALIGN_TOP_LEFT, 4, 2);

    hint_label_ = lv_label_create(panel_);
    lv_obj_set_style_text_font(hint_label_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(hint_label_, theme->text_color(), 0);
    lv_obj_set_style_text_opa(hint_label_, LV_OPA_60, 0);
    lv_label_set_text(hint_label_, "OK add  |  2x del  |  HOLD go");
    lv_obj_align(hint_label_, LV_ALIGN_TOP_LEFT, 4, 34);

    list_ = lv_obj_create(panel_);
    lv_obj_set_size(list_, LV_PCT(100), h - 12 - 76);
    lv_obj_align(list_, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_, 0, 0);
    lv_obj_set_style_pad_all(list_, 0, 0);
    lv_obj_set_style_pad_row(list_, 2, 0);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    rows_.clear();
    for (const auto& cell : cells_) {
        auto* r = lv_obj_create(list_);
        lv_obj_set_size(r, LV_PCT(100), 26);
        lv_obj_set_style_radius(r, 0, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);

        auto* tx = lv_label_create(r);
        lv_obj_set_style_text_font(tx, theme->text_font()->font(), 0);
        lv_obj_set_style_text_color(tx, theme->text_color(), 0);
        if (cell.type == 1 || cell.type == 2) {
            lv_obj_set_style_text_opa(tx, LV_OPA_80, 0);
        }
        lv_label_set_text(tx, cell.label.c_str());
        lv_obj_center(tx);
        rows_.push_back(r);
    }
    ESP_LOGI(TAG, "text writer built (%u cells)", (unsigned)cells_.size());
}

void TextWriter::UpdateText() {
    if (text_label_ != nullptr) {
        lv_label_set_text(text_label_, text_.empty() ? "..." : text_.c_str());
    }
}

void TextWriter::UpdateSelection() {
    if (rows_.empty()) {
        return;
    }
    auto* theme = ThemeOf(display_);
    if (theme == nullptr) {
        return;
    }
    int count = static_cast<int>(rows_.size());
    selected_ = ((selected_ % count) + count) % count;
    for (int i = 0; i < count; i++) {
        bool sel = i == selected_;
        lv_obj_set_style_bg_opa(rows_[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (sel) {
            lv_obj_set_style_bg_color(rows_[i], lv_color_hex(0xFFB020), 0);
            // Selected row paints its label dark on the amber highlight.
            lv_obj_set_style_text_color(lv_obj_get_child(rows_[i], 0), lv_color_hex(0x14100A), 0);
        } else {
            // letters cool, digits warm, actions distinct
            const auto& cell = cells_[i];
            uint32_t hue = cell.type == 0 ? 0x4FC3F7 : cell.type == 1 ? 0xFF8A65 : 0x81C784;
            lv_obj_set_style_text_color(lv_obj_get_child(rows_[i], 0), lv_color_hex(hue), 0);
        }
    }
    lv_obj_scroll_to_view(rows_[selected_], LV_ANIM_ON);
}

void TextWriter::MoveSelection(int delta) {
    if (!open_ || rows_.empty()) {
        return;
    }
    selected_ += delta;
    DisplayLockGuard lock(display_);
    UpdateSelection();
}

void TextWriter::Insert() {
    if (!open_ || rows_.empty()) {
        return;
    }
    const auto& cell = cells_[selected_];
    switch (cell.type) {
        case 0:
            text_ += cell.label;
            break;
        case 1:
            if (!text_.empty()) {
                text_.pop_back();
            }
            break;
        case 2:
            Submit();
            return;
    }
    DisplayLockGuard lock(display_);
    UpdateText();
}

void TextWriter::Backspace() {
    if (!open_ || text_.empty()) {
        return;
    }
    text_.pop_back();
    DisplayLockGuard lock(display_);
    UpdateText();
}

void TextWriter::Submit() {
    if (!open_) {
        return;
    }
    ESP_LOGI(TAG, "submitted: %s", text_.c_str());
    auto text = text_;
    Close();
    if (on_submit_) {
        on_submit_(text);
    }
}

void TextWriter::Cancel() {
    if (!open_) {
        return;
    }
    Close();
    if (on_cancel_) {
        on_cancel_();
    }
}
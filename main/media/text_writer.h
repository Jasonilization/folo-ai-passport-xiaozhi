#ifndef TEXT_WRITER_H
#define TEXT_WRITER_H

#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <functional>
#include <string>
#include <vector>

// Full-screen text entry overlay, designed for the AI Passport's 3 physical
// buttons (up / down / OK):
//   up/down  move through the character table (wraps, scrolls)
//   OK click insert the selected character
//   OK double backspace
//   OK hold  submit
//
// The table ends with two action rows: <DEL> and <GO>.
class TextWriter {
public:
    using SubmitCb = std::function<void(const std::string&)>;
    using CancelCb = std::function<void()>;

    explicit TextWriter(LcdDisplay* display) : display_(display) {}

    void SetSubmitCallback(SubmitCb cb) { on_submit_ = std::move(cb); }
    void SetCancelCallback(CancelCb cb) { on_cancel_ = std::move(cb); }

    bool IsOpen() const { return open_; }

    void Open(const std::string& initial = "");
    void Close();
    void MoveSelection(int delta);  // -1 = up, +1 = down
    void Insert();
    void Backspace();
    void Submit();
    void Cancel();

    const std::string& text() const { return text_; }

private:
    void Build();
    void UpdateText();
    void UpdateSelection();

    struct Cell {
        std::string label;
        int type = 0;  // 0 = char, 1 = delete, 2 = submit
    };

    LcdDisplay* display_ = nullptr;
    bool open_ = false;
    std::string text_;
    int selected_ = 0;
    std::vector<Cell> cells_;

    SubmitCb on_submit_;
    CancelCb on_cancel_;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* text_label_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* list_ = nullptr;
    std::vector<lv_obj_t*> rows_;
};

#endif
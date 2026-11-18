#include "media_mode.h"

#include "application.h"
#include "archive_client.h"

#include <cJSON.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <material_symbols.h>

#define TAG "MediaMode"

namespace {
LvglTheme* ThemeOf(LcdDisplay* display) {
    return static_cast<LvglTheme*>(display->GetTheme());
}
constexpr uint32_t kAccent = 0xFFB020;
lv_color_t TagHue(int tag) {
    switch (tag) {
        case 1: return lv_color_hex(0x4FC3F7);   // search
        case 2: return lv_color_hex(0xFF8A65);   // voice
        case 3: return lv_color_hex(0x81C784);   // type
        case 4: return lv_color_hex(0xBA68C8);   // quick album
        case 5: return lv_color_hex(0xF06292);   // search history
        case 6: return lv_color_hex(0xFFF176);   // recent files
        case 7: return lv_color_hex(0x4DB6AC);   // played
        case 8: return lv_color_hex(0xFFB020);   // radio
        case 9: return lv_color_hex(0xFF7043);   // resume
        case 10: return lv_color_hex(0x90A4AE);  // exit
        default: return lv_color_hex(0x90A4AE);  // back
    }
}
}  // namespace

MediaMode::MediaMode(LcdDisplay* display, AudioCodec* codec)
    : display_(display), codec_(codec) {
    player_ = std::make_unique<MusicPlayer>(codec);
    player_->SetCallback([this](MusicPlayer::Event event, const MediaTrack& track, int arg) {
        OnPlayerEvent(event, track, arg);
    });
    writer_ = std::make_unique<TextWriter>(display);
    writer_->SetSubmitCallback([this](const std::string& text) {
        if (!text.empty()) {
            StartSearch(text);
        } else {
            GoRoot();
        }
    });
    writer_->SetCancelCallback([this]() { GoRoot(); });
    // Voice search: the STT text of the user's speech becomes the query.
    Application::GetInstance().SetOnSttText([this](const std::string& text) {
        OnSttText(text);
    });
    LoadHistory();
}

MediaMode::~MediaMode() {
    Close();
    if (repeat_timer_ != nullptr) {
        lv_timer_delete(repeat_timer_);
        repeat_timer_ = nullptr;
    }
}

void MediaMode::Open() {
    if (display_ == nullptr) {
        return;
    }
    // Drop the assistant's MQTT protocol entirely while browsing: on the C3
    // (400KB RAM, no PSRAM) an HTTP stream running concurrently with the MQTT
    // socket corrupts the heap (tlsf double-free crashes). Voice search and
    // exiting media mode re-create the protocol via InitializeProtocol().
    Application::GetInstance().ResetProtocol();
    open_ = true;
    state_ = State::kRoot;
    DisplayLockGuard lock(display_);
    if (panel_ == nullptr) {
        BuildPanel();
        if (panel_ == nullptr) {
            return;
        }
    } else {
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(np_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(list_, LV_OBJ_FLAG_HIDDEN);
    }
    GoRoot();
}

void MediaMode::Close() {
    open_ = false;
    if (player_ != nullptr) {
        player_->Stop();
    }
    // Restore the assistant's protocol so chat works after exiting music.
    Application::GetInstance().ReconnectProtocol();
    if (writer_ != nullptr && writer_->IsOpen()) {
        writer_->Close();
    }
    if (panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(np_panel_, LV_OBJ_FLAG_HIDDEN);
    // Invalidate any in-flight fetch so late results are dropped.
    items_.clear();
    files_.clear();
    stations_.clear();
}

void MediaMode::BuildPanel() {
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
    lv_label_set_text(title_label_, "MUSIC");
    lv_obj_align(title_label_, LV_ALIGN_TOP_LEFT, 4, 2);

    hint_label_ = lv_obj_create(panel_);
    lv_obj_set_size(hint_label_, LV_PCT(100), 18);
    lv_obj_set_style_bg_opa(hint_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hint_label_, 0, 0);
    lv_obj_set_style_pad_all(hint_label_, 0, 0);
    lv_obj_clear_flag(hint_label_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hint_label_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint_label_, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(hint_label_, LV_ALIGN_TOP_LEFT, 4, 28);
    auto* hint_left = lv_label_create(hint_label_);
    lv_obj_set_style_text_font(hint_left, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(hint_left, theme->text_color(), 0);
    lv_obj_set_style_text_opa(hint_left, LV_OPA_60, 0);
    lv_label_set_text(hint_left, "OK open | HOLD exit");
    auto* hint_right = lv_label_create(hint_label_);
    lv_obj_set_style_text_font(hint_right, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(hint_right, theme->text_color(), 0);
    lv_obj_set_style_text_opa(hint_right, LV_OPA_60, 0);
    lv_label_set_text(hint_right, "2x back");

    list_ = lv_obj_create(panel_);
    lv_obj_set_size(list_, LV_PCT(100), h - 12 - 60);
    lv_obj_align(list_, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_, 0, 0);
    lv_obj_set_style_pad_all(list_, 0, 0);
    lv_obj_set_style_pad_row(list_, 2, 0);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Now-playing overlay.
    np_panel_ = lv_obj_create(panel_);
    lv_obj_set_size(np_panel_, LV_PCT(100), h - 12 - 60);
    lv_obj_align(np_panel_, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_opa(np_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(np_panel_, 0, 0);
    lv_obj_set_style_pad_all(np_panel_, 0, 0);
    lv_obj_set_style_pad_row(np_panel_, 6, 0);
    lv_obj_set_flex_flow(np_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(np_panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(np_panel_, LV_OBJ_FLAG_HIDDEN);

    np_title_ = lv_label_create(np_panel_);
    lv_obj_set_style_text_font(np_title_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(np_title_, theme->text_color(), 0);
    lv_label_set_long_mode(np_title_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(np_title_, LV_PCT(100));
    lv_label_set_text(np_title_, "");

    np_artist_ = lv_label_create(np_panel_);
    lv_obj_set_style_text_font(np_artist_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(np_artist_, theme->text_color(), 0);
    lv_obj_set_style_text_opa(np_artist_, LV_OPA_70, 0);
    lv_label_set_long_mode(np_artist_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(np_artist_, LV_PCT(100));
    lv_label_set_text(np_artist_, "");

    np_state_ = lv_label_create(np_panel_);
    lv_obj_set_style_text_font(np_state_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(np_state_, theme->text_color(), 0);
    lv_label_set_text(np_state_, "OK pause | 2x stop | HOLD exit");
}

void MediaMode::GoRoot() {
    if (writer_->IsOpen()) {
        writer_->Close();
    }
    std::vector<Row> rows;
    rows.push_back({kTagSearch, 0, "Search songs", MATERIAL_SYMBOLS_SEARCH});
    rows.push_back({kTagRadio, 0, "Internet radio", MATERIAL_SYMBOLS_HEADPHONES});
    if (!history_.empty()) {
        rows.push_back({kTagSearchHistory, 0, "Search history", MATERIAL_SYMBOLS_SCHEDULE});
    }
    if (!played_.empty()) {
        rows.push_back({kTagFilesHistory, 0, "Recent files", MATERIAL_SYMBOLS_MUSIC_NOTE});
    }
    if (has_track_) {
        rows.push_back({kTagResume, 0, "Now playing", MATERIAL_SYMBOLS_MUSIC_NOTE});
    }
    rows.push_back({kTagExit, 0, "Exit", MATERIAL_SYMBOLS_CLOSE});
    state_ = State::kRoot;
    RebuildRows(rows, "MUSIC");
}

void MediaMode::RebuildRows(const std::vector<Row>& rows, const std::string& title) {
    if (panel_ == nullptr) {
        return;
    }
    rows_ = rows;
    selected_ = 0;

    DisplayLockGuard lock(display_);
    lv_obj_add_flag(np_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_, LV_OBJ_FLAG_HIDDEN);
    if (title_label_ != nullptr) {
        lv_label_set_text(title_label_, title.c_str());
    }
    lv_obj_clean(list_);
    row_objs_.clear();
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);

    auto* theme = ThemeOf(display_);
    if (theme == nullptr) {
        return;
    }
    for (size_t i = 0; i < rows_.size(); i++) {
        const auto& row = rows_[i];
        auto* r = lv_obj_create(list_);
        lv_obj_set_size(r, LV_PCT(100), 30);
        lv_obj_set_style_radius(r, 0, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(r, 10, 0);
        lv_obj_set_style_pad_right(r, 10, 0);
        lv_obj_set_style_pad_column(r, 8, 0);

        auto* ic = lv_label_create(r);
        lv_obj_set_style_text_font(ic, theme->icon_font()->font(), 0);
        lv_obj_set_style_text_color(ic, TagHue(row.tag), 0);
        lv_label_set_text(ic, row.icon.c_str());

        auto* tx = lv_label_create(r);
        lv_obj_set_style_text_font(tx, theme->text_font()->font(), 0);
        lv_obj_set_style_text_color(tx, theme->text_color(), 0);
        if (row.tag == kTagBack) {
            lv_obj_set_style_text_opa(ic, LV_OPA_60, 0);
            lv_obj_set_style_text_opa(tx, LV_OPA_60, 0);
        }
        lv_label_set_text(tx, row.label.c_str());
        row_objs_.push_back(r);
    }
    UpdateSelection();
}

void MediaMode::UpdateSelection() {
    if (panel_ == nullptr || row_objs_.empty()) {
        return;
    }
    int count = static_cast<int>(row_objs_.size());
    selected_ = ((selected_ % count) + count) % count;
    auto* theme = ThemeOf(display_);
    if (theme == nullptr) {
        return;
    }
    for (int i = 0; i < count; i++) {
        bool sel = i == selected_;
        lv_obj_set_style_bg_opa(row_objs_[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (sel) {
            lv_obj_set_style_bg_color(row_objs_[i], lv_color_hex(kAccent), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(row_objs_[i], 0), lv_color_hex(0x14100A), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(row_objs_[i], 1), lv_color_hex(0x14100A), 0);
        } else {
            lv_obj_set_style_text_color(lv_obj_get_child(row_objs_[i], 0), TagHue(rows_[i].tag), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(row_objs_[i], 1), theme->text_color(), 0);
        }
    }
    lv_obj_scroll_to_view(row_objs_[selected_], LV_ANIM_ON);
}

void MediaMode::ShowList(const std::string& title) {
    if (panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_add_flag(np_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_, LV_OBJ_FLAG_HIDDEN);
    if (title_label_ != nullptr) {
        lv_label_set_text(title_label_, title.c_str());
    }
}

void MediaMode::OnUp() {
    if (state_ == State::kTyping) {
        writer_->MoveSelection(-1);
        return;
    }
    if (state_ == State::kPlaying) {
        // Volume up while playing.
        if (codec_ != nullptr) {
            codec_->SetOutputVolume(codec_->output_volume() + 10);
            UpdateNowPlaying();
        }
        return;
    }
    selected_--;
    DisplayLockGuard lock(display_);
    UpdateSelection();
}

void MediaMode::OnDown() {
    if (state_ == State::kTyping) {
        writer_->MoveSelection(1);
        return;
    }
    if (state_ == State::kPlaying) {
        if (codec_ != nullptr) {
            codec_->SetOutputVolume(codec_->output_volume() - 10);
            UpdateNowPlaying();
        }
        return;
    }
    selected_++;
    DisplayLockGuard lock(display_);
    UpdateSelection();
}

void MediaMode::OnOkClick() {
    if (state_ == State::kTyping) {
        writer_->Insert();
        return;
    }
    if (state_ == State::kPlaying) {
        player_->TogglePaused();
        UpdateNowPlaying();
        return;
    }
    ActivateRow(selected_);
}

void MediaMode::OnOkDouble() {
    if (state_ == State::kTyping) {
        writer_->Backspace();
        return;
    }
    if (state_ == State::kSearchHub) {
        // Speak instead of typing (up+down chord is not reliably readable on
        // the single-ADC resistor ladder, so double-OK triggers voice here).
        StartVoiceSearch();
        return;
    }
    if (state_ == State::kPlaying) {
        player_->Stop();
        GoRoot();
        return;
    }
    Back();
}

void MediaMode::OnOkHold() {
    if (state_ == State::kTyping) {
        writer_->Submit();
        return;
    }
    Close();
}

void MediaMode::OnUpHoldStart() {
    OnUp();
    repeat_direction_ = -1;
    if (repeat_timer_ == nullptr) {
        repeat_timer_ = lv_timer_create(OnTimer, 70, this);
    } else {
        lv_timer_resume(repeat_timer_);
    }
}

void MediaMode::OnDownHoldStart() {
    OnDown();
    repeat_direction_ = 1;
    if (repeat_timer_ == nullptr) {
        repeat_timer_ = lv_timer_create(OnTimer, 70, this);
    } else {
        lv_timer_resume(repeat_timer_);
    }
}

void MediaMode::OnHoldStop() {
    repeat_direction_ = 0;
    if (repeat_timer_ != nullptr) {
        lv_timer_pause(repeat_timer_);
    }
}

void MediaMode::OnTimer(lv_timer_t* timer) {
    auto* self = static_cast<MediaMode*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->Repeat();
    }
}

void MediaMode::Repeat() {
    if (repeat_direction_ == 0) {
        return;
    }
    if (repeat_direction_ < 0) {
        OnUp();
    } else {
        OnDown();
    }
}

void MediaMode::ActivateRow(int position) {
    if (position < 0 || position >= static_cast<int>(rows_.size())) {
        return;
    }
    const Row& row = rows_[position];
    switch (row.tag) {
        case kTagExit:
            Close();
            break;
        case kTagSearch:
            ShowSearchHub();
            break;
        case kTagTypeSearch:
            state_ = State::kTyping;
            writer_->Open("");
            break;
        case kTagQuick: {
            static const char* kQuickIds[] = {"undertaleost_202004", "deltarune-soundtrack"};
            static const char* kQuickTitles[] = {"Undertale OST", "Deltarune OST"};
            if (row.index < 0 || row.index > 1) return;
            MediaItem item;
            item.identifier = kQuickIds[row.index];
            item.title = kQuickTitles[row.index];
            OpenItem(item);
            break;
        }
        case kTagVoiceSearch:
            StartVoiceSearch();
            break;
        case kTagSearchHistory: {
            state_ = State::kSearchHistory;
            std::vector<Row> rows;
            rows.push_back({kTagBack, 0, "Back", MATERIAL_SYMBOLS_ARROW_BACK});
            for (size_t i = 0; i < history_.size(); i++) {
                rows.push_back({kTagHistory, (int)i, history_[i], MATERIAL_SYMBOLS_SCHEDULE});
            }
            RebuildRows(rows, "SEARCH HISTORY");
            break;
        }
        case kTagFilesHistory: {
            state_ = State::kFilesHistory;
            std::vector<Row> rows;
            rows.push_back({kTagBack, 0, "Back", MATERIAL_SYMBOLS_ARROW_BACK});
            for (size_t i = 0; i < played_.size(); i++) {
                rows.push_back({kTagPlayed, (int)i, played_[i].title,
                                MATERIAL_SYMBOLS_MUSIC_NOTE});
            }
            RebuildRows(rows, "RECENT FILES");
            break;
        }
        case kTagHistory: {
            if (row.index < 0 || row.index >= static_cast<int>(history_.size())) {
                return;
            }
            StartSearch(history_[row.index]);
            break;
        }
        case kTagPlayed: {
            if (row.index < 0 || row.index >= static_cast<int>(played_.size())) {
                return;
            }
            PlayTrack(played_[row.index]);
            break;
        }
        case kTagRadio: {
            state_ = State::kStations;
            auto* fetch = new FetchStations{this, "lofi"};
            xTaskCreate(FetchStationsTask, "radio_fetch", 10240, fetch, 5, nullptr);
            RebuildRows({}, "RADIO");
            break;
        }
        case kTagResume:
            if (has_track_) {
                PlayTrack(last_track_);
            }
            break;
        case kTagBack:
            Back();
            break;
        case kTagItem: {
            if (row.index < 0 || row.index >= static_cast<int>(items_.size())) {
                return;
            }
            OpenItem(items_[row.index]);
            break;
        }
        case kTagFile: {
            if (row.index < 0 || row.index >= static_cast<int>(files_.size())) {
                return;
            }
            current_file_index_ = row.index;
            MediaTrack track;
            track.title = files_[row.index].name;
            track.artist = "";
            track.url = files_[row.index].url;
            PlayTrack(track);
            break;
        }
        case kTagStation: {
            if (row.index < 0 || row.index >= static_cast<int>(stations_.size())) {
                return;
            }
            PlayTrack(stations_[row.index]);
            break;
        }
    }
}

void MediaMode::Back() {
    switch (state_) {
        case State::kItems:
        case State::kStations:
        case State::kSearchHistory:
        case State::kFilesHistory:
            GoRoot();
            break;
        case State::kFiles:
            // Back to the item's file list = the items screen.
            state_ = State::kItems;
            {
                std::vector<Row> rows;
                rows.push_back({kTagBack, 0, "Back", MATERIAL_SYMBOLS_ARROW_BACK});
                for (size_t i = 0; i < items_.size(); i++) {
                    rows.push_back({kTagItem, (int)i, items_[i].title, MATERIAL_SYMBOLS_SD_CARD});
                }
                RebuildRows(rows, "SONGS");
            }
            break;
        default:
            GoRoot();
            break;
    }
}

void MediaMode::ShowSearchHub(const std::string& title) {
    std::vector<Row> rows;
    rows.push_back({kTagVoiceSearch, 0, "Speak a song", MATERIAL_SYMBOLS_MIC});
    rows.push_back({kTagTypeSearch, 0, "Type a song", MATERIAL_SYMBOLS_SEARCH});
    // Preloaded albums (verified archive.org identifiers) so favorite OSTs are
    // one tap away without searching.
    rows.push_back({kTagQuick, 0, "Undertale OST", MATERIAL_SYMBOLS_MUSIC_NOTE});
    rows.push_back({kTagQuick, 1, "Deltarune OST", MATERIAL_SYMBOLS_MUSIC_NOTE});
    rows.push_back({kTagBack, 0, "Back", MATERIAL_SYMBOLS_ARROW_BACK});
    state_ = State::kSearchHub;
    RebuildRows(rows, title.empty() ? "SEARCH" : title);
}

void MediaMode::StartSearch(const std::string& query) {
    // Record in search history (most recent first, deduped, capped at 8).
    for (auto it = history_.begin(); it != history_.end(); ++it) {
        if (*it == query) {
            history_.erase(it);
            break;
        }
    }
    history_.insert(history_.begin(), query);
    if (history_.size() > 8) {
        history_.pop_back();
    }
    SaveHistory();
    state_ = State::kItems;
    auto* fetch = new FetchItems{this, query};
    xTaskCreate(FetchItemsTask, "ia_search", 10240, fetch, 5, nullptr);
    RebuildRows({}, "SEARCH: " + query);
}

void MediaMode::StartVoiceSearch() {
    if (player_ != nullptr && player_->IsPlaying()) {
        player_->Stop();
    }
    voice_search_pending_ = true;
    // Stay on the search interface; the title line shows the listening state
    // instead of a fake list entry.
    state_ = State::kSearchHub;
    ShowSearchHub("LISTENING...");
    // Media mode dropped the protocol; re-create it, give it a moment to
    // connect, then start listening.
    Application::GetInstance().ReconnectProtocol();
    xTaskCreate(
        [](void* arg) {
            vTaskDelay(pdMS_TO_TICKS(4000));
            Application::GetInstance().Schedule(
                []() { Application::GetInstance().StartListening(); });
            vTaskDelete(nullptr);
        },
        "vs_reconnect", 4096, nullptr, 3, nullptr);
    // If no speech is recognized within 12 s, bail out instead of hanging.
    xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<MediaMode*>(arg);
            vTaskDelay(pdMS_TO_TICKS(30000));
            Application::GetInstance().Schedule([self]() { self->VoiceSearchTimeout(); });
            vTaskDelete(nullptr);
        },
        "vs_timeout", 3072, this, 3, nullptr);
}

void MediaMode::VoiceSearchTimeout() {
    if (!voice_search_pending_ || !open_) {
        return;
    }
    voice_search_pending_ = false;
    Application::GetInstance().StopListening();
    state_ = State::kSearchHub;
    ShowSearchHub("NO SPEECH HEARD");
}

void MediaMode::OnSttText(const std::string& text) {
    if (!voice_search_pending_ || !open_) {
        return;
    }
    if (text.empty()) {
        // Server sometimes sends an empty STT right after the channel opens;
        // keep listening instead of giving up.
        ESP_LOGI(TAG, "empty STT, still listening for a song...");
        return;
    }
    voice_search_pending_ = false;
    ESP_LOGI(TAG, "voice search query: %s", text.c_str());
    Application::GetInstance().StopListening();
    // Free the audio channel's heap before the HTTPS search.
    Application::GetInstance().CloseAudioChannel();
    StartSearch(text);
}

void MediaMode::OpenItem(const MediaItem& item) {
    state_ = State::kFiles;
    auto* fetch = new FetchFiles{this, item.identifier, item.title};
    xTaskCreate(FetchFilesTask, "ia_files", 10240, fetch, 5, nullptr);
    RebuildRows({}, "LOADING");
}

void MediaMode::FetchItemsTask(void* arg) {
    auto* fetch = static_cast<FetchItems*>(arg);
    auto items = std::make_shared<std::vector<MediaItem>>();
    auto status = std::make_shared<int>(kFetchError);
    ArchiveClient client;
    bool ok = client.Search(fetch->query, 20, *items);
    if (!ok) {
        // One retry: boot-time TLS sessions (OTA/MQTT) can starve the heap.
        vTaskDelay(pdMS_TO_TICKS(4000));
        ok = client.Search(fetch->query, 20, *items);
    }
    if (ok) {
        *status = items->empty() ? kFetchEmpty : kFetchOk;
    } else {
        ESP_LOGW(TAG, "item search failed for \"%s\"", fetch->query.c_str());
    }
    auto* self = fetch->self;
    delete fetch;
    Application::GetInstance().Schedule([self, items, status]() {
        if (!self->open_) {
            return;
        }
        self->items_ = *items;
        ESP_LOGI(TAG, "search results: %u items", (unsigned)items->size());
        std::vector<Row> rows;
        rows.push_back({kTagBack, 0, "Back", MATERIAL_SYMBOLS_ARROW_BACK});
        for (size_t i = 0; i < items->size(); i++) {
            rows.push_back({kTagItem, (int)i, (*items)[i].title, MATERIAL_SYMBOLS_SD_CARD});
        }
        if (*status == kFetchError) {
            rows.push_back({kTagBack, 0, "Search failed - check WiFi",
                            MATERIAL_SYMBOLS_WARNING});
        } else if (items->empty()) {
            rows.push_back({kTagBack, 0, "No results", MATERIAL_SYMBOLS_CLOSE});
        }
        self->RebuildRows(rows, "SONGS");
    });
    vTaskDelete(nullptr);
}

void MediaMode::FetchFilesTask(void* arg) {
    auto* fetch = static_cast<FetchFiles*>(arg);
    auto files = std::make_shared<std::vector<MediaFile>>();
    auto status = std::make_shared<int>(kFetchError);
    ArchiveClient client;
    bool ok = client.ListFiles(fetch->identifier, *files);
    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(4000));
        ok = client.ListFiles(fetch->identifier, *files);
    }
    if (ok) {
        *status = files->empty() ? kFetchEmpty : kFetchOk;
    } else {
        ESP_LOGW(TAG, "file listing failed for %s", fetch->identifier.c_str());
    }
    auto* self = fetch->self;
    std::string title = fetch->title;
    delete fetch;
    Application::GetInstance().Schedule([self, files, title, status]() {
        if (!self->open_) {
            return;
        }
        self->files_ = *files;
        ESP_LOGI(TAG, "file list: %u entries", (unsigned)files->size());
        for (size_t i = 0; i < files->size() && i < 5; i++) {
            ESP_LOGI(TAG, "  file: %s", (*files)[i].name.c_str());
        }
        std::vector<Row> rows;
        rows.push_back({kTagBack, 0, "Back", MATERIAL_SYMBOLS_ARROW_BACK});
        for (size_t i = 0; i < files->size(); i++) {
            rows.push_back({kTagFile, (int)i, (*files)[i].name, MATERIAL_SYMBOLS_MUSIC_NOTE});
        }
        if (*status == kFetchError) {
            rows.push_back({kTagBack, 0, "Load failed - check WiFi",
                            MATERIAL_SYMBOLS_WARNING});
        } else if (files->empty()) {
            rows.push_back({kTagBack, 0, "No audio files", MATERIAL_SYMBOLS_CLOSE});
        }
        self->RebuildRows(rows, title);
    });
    vTaskDelete(nullptr);
}

void MediaMode::FetchStationsTask(void* arg) {
    auto* fetch = static_cast<FetchStations*>(arg);
    auto stations = std::make_shared<std::vector<MediaTrack>>();
    auto status = std::make_shared<int>(kFetchError);
    RadioClient client;
    bool ok = client.Search(fetch->query, 20, *stations);
    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(4000));
        ok = client.Search(fetch->query, 20, *stations);
    }
    if (ok) {
        *status = stations->empty() ? kFetchEmpty : kFetchOk;
    } else {
        ESP_LOGW(TAG, "station search failed for \"%s\"", fetch->query.c_str());
    }
    auto* self = fetch->self;
    delete fetch;
    Application::GetInstance().Schedule([self, stations, status]() {
        if (!self->open_) {
            return;
        }
        self->stations_ = *stations;
        ESP_LOGI(TAG, "radio results: %u stations", (unsigned)stations->size());
        std::vector<Row> rows;
        rows.push_back({kTagBack, 0, "Back", MATERIAL_SYMBOLS_ARROW_BACK});
        for (size_t i = 0; i < stations->size(); i++) {
            rows.push_back({kTagStation, (int)i, (*stations)[i].title, MATERIAL_SYMBOLS_HEADPHONES});
        }
        if (*status == kFetchError) {
            rows.push_back({kTagBack, 0, "Radio failed - check WiFi",
                            MATERIAL_SYMBOLS_WARNING});
        } else if (stations->empty()) {
            rows.push_back({kTagBack, 0, "No stations found", MATERIAL_SYMBOLS_CLOSE});
        }
        self->RebuildRows(rows, "RADIO");
    });
    vTaskDelete(nullptr);
}

void MediaMode::PlayTrack(const MediaTrack& track) {
    last_track_ = track;
    has_track_ = true;
    // Record in the recent-files history (dedup by URL, cap 16).
    for (auto it = played_.begin(); it != played_.end(); ++it) {
        if (it->url == track.url) {
            played_.erase(it);
            break;
        }
    }
    played_.insert(played_.begin(), track);
    if (played_.size() > 16) {
        played_.pop_back();
    }
    SaveHistory();
    state_ = State::kPlaying;
    player_->Play(track);
    ShowNowPlaying();
}

void MediaMode::ShowNowPlaying() {
    if (panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(np_panel_, LV_OBJ_FLAG_HIDDEN);
    if (title_label_ != nullptr) {
        lv_label_set_text(title_label_, "NOW PLAYING");
    }
    UpdateNowPlaying();
}

void MediaMode::UpdateNowPlaying() {
    if (np_title_ == nullptr) {
        return;
    }
    lv_label_set_text(np_title_, last_track_.title.c_str());
    lv_label_set_text(np_artist_, last_track_.artist.c_str());
    const char* state = "Playing";
    if (player_->IsPaused()) {
        state = "Paused";
    } else if (!player_->IsPlaying()) {
        state = "Stopped";
    }
    char hint[64];
    snprintf(hint, sizeof(hint), "%s | vol %d", state,
             codec_ != nullptr ? codec_->output_volume() : 0);
    lv_label_set_text(np_state_, hint);
}

void MediaMode::OnPlayerEvent(MusicPlayer::Event event, const MediaTrack& track, int arg) {
    (void)track;
    (void)arg;
    if (!open_) {
        return;
    }
    switch (event) {
        case MusicPlayer::Event::kStarted:
            ESP_LOGI(TAG, "playback started: %s", track.title.c_str());
            if (state_ == State::kPlaying) {
                UpdateNowPlaying();
            }
            break;
        case MusicPlayer::Event::kProgress:
            if (state_ == State::kPlaying) {
                UpdateNowPlaying();
            }
            break;
        case MusicPlayer::Event::kFinished:
            ESP_LOGI(TAG, "playback finished");
            // Playlist loop: within a folder, keep playing the next track
            // (wrapping around) instead of dropping back to the menu.
            if (current_file_index_ >= 0 && !files_.empty()) {
                int next = (current_file_index_ + 1) % static_cast<int>(files_.size());
                ESP_LOGI(TAG, "loop: next track %d/%u", next,
                         (unsigned)files_.size());
                current_file_index_ = next;
                MediaTrack track;
                track.title = files_[next].name;
                track.url = files_[next].url;
                PlayTrack(track);
            } else if (state_ == State::kPlaying) {
                GoRoot();
            }
            break;
        case MusicPlayer::Event::kError:
            ESP_LOGE(TAG, "playback error");
            if (state_ == State::kPlaying) {
                lv_label_set_text(np_state_, "Cannot play this file");
            }
            break;
        default:
            break;
    }
}
void MediaMode::SaveHistory() {
    cJSON* root = cJSON_CreateObject();
    cJSON* q = cJSON_CreateArray();
    for (const auto& item : history_) {
        cJSON_AddItemToArray(q, cJSON_CreateString(item.c_str()));
    }
    cJSON_AddItemToObject(root, "queries", q);
    cJSON* t = cJSON_CreateArray();
    for (const auto& tr : played_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "title", tr.title.c_str());
        cJSON_AddStringToObject(o, "url", tr.url.c_str());
        cJSON_AddStringToObject(o, "artist", tr.artist.c_str());
        cJSON_AddNumberToObject(o, "duration", tr.duration_sec);
        cJSON_AddBoolToObject(o, "stream", tr.is_stream);
        cJSON_AddItemToArray(t, o);
    }
    cJSON_AddItemToObject(root, "tracks", t);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("media", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "history", json);
        nvs_commit(h);
        nvs_close(h);
    }
    free(json);
}

void MediaMode::LoadHistory() {
    nvs_handle_t h;
    if (nvs_open("media", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_str(h, "history", nullptr, &len) != ESP_OK || len == 0 || len > 8192) {
        nvs_close(h);
        return;
    }
    std::string buf(len, '\0');
    if (nvs_get_str(h, "history", buf.data(), &len) != ESP_OK) {
        nvs_close(h);
        return;
    }
    nvs_close(h);
    cJSON* root = cJSON_Parse(buf.c_str());
    if (root == nullptr) {
        return;
    }
    cJSON* q = cJSON_GetObjectItem(root, "queries");
    if (cJSON_IsArray(q)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, q) {
            if (cJSON_IsString(it) && history_.size() < 8) {
                history_.push_back(it->valuestring);
            }
        }
    }
    cJSON* t = cJSON_GetObjectItem(root, "tracks");
    if (cJSON_IsArray(t)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, t) {
            if (!cJSON_IsObject(it) || played_.size() >= 16) {
                continue;
            }
            MediaTrack tr;
            cJSON* v = cJSON_GetObjectItem(it, "title");
            if (cJSON_IsString(v)) tr.title = v->valuestring;
            v = cJSON_GetObjectItem(it, "url");
            if (cJSON_IsString(v)) tr.url = v->valuestring;
            v = cJSON_GetObjectItem(it, "artist");
            if (cJSON_IsString(v)) tr.artist = v->valuestring;
            v = cJSON_GetObjectItem(it, "duration");
            if (cJSON_IsNumber(v)) tr.duration_sec = v->valueint;
            v = cJSON_GetObjectItem(it, "stream");
            tr.is_stream = cJSON_IsTrue(v);
            if (!tr.url.empty()) {
                played_.push_back(std::move(tr));
            }
        }
    }
    cJSON_Delete(root);
}

void MediaMode::DebugSearch(const std::string& q) {
    Open();
    StartSearch(q);
}

void MediaMode::DebugList(const std::string& identifier) {
    Open();
    MediaItem item;
    item.identifier = identifier;
    item.title = identifier;
    OpenItem(item);
}

void MediaMode::DebugPlay(const std::string& url, int seconds) {
    Open();  // drops MQTT for heap, like the real UI flow
    MediaTrack track;
    track.title = "debug stream";
    track.url = url;
    PlayTrack(track);
    xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<MediaMode*>(arg);
            int secs = 6;
            // arg is self; seconds passed via a static is ugly - use 6s default
            vTaskDelay(pdMS_TO_TICKS(secs * 1000));
            Application::GetInstance().Schedule([self]() {
                if (self->player_ != nullptr) {
                    self->player_->Stop();
                }
            });
            vTaskDelete(nullptr);
        },
        "dbg_stop", 3072, this, 3, nullptr);
}

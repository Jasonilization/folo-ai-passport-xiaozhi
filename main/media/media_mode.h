#ifndef MEDIA_MODE_H
#define MEDIA_MODE_H

#include "audio/audio_codec.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "media/media_types.h"
#include "media/music_player.h"
#include "media/radio_client.h"
#include "media/text_writer.h"

#include <memory>
#include <string>
#include <vector>

// Full-screen media browser + player for the AI Passport.
//
// Screens: root menu -> archive.org search results -> folder file list -> play,
// or internet radio stations -> play, with a now-playing screen while audio
// runs. All HTTP work happens on short-lived tasks that marshal results back
// onto the main loop, so the UI never blocks.
class MediaMode {
public:
    MediaMode(LcdDisplay* display, AudioCodec* codec);
    ~MediaMode();

    void Open();   // show the root screen
    void Close();  // hide everything and stop playback
    bool IsOpen() const { return open_; }

    // Button routing: the board forwards buttons here while media mode is open.
    void OnUp();
    void OnDown();
    void OnOkClick();
    void OnOkDouble();
    void OnOkHold();
    // Held up/down repeats the action (fast scrolling through the keyboard
    // column and long lists); OnHoldStop ends the repeat.
    void OnUpHoldStart();
    void OnDownHoldStart();
    void OnHoldStop();

private:
    static void OnTimer(lv_timer_t* timer);
    void Repeat();

    enum class State { kRoot, kSearchHub, kSearchHistory, kFilesHistory, kItems, kFiles,
                       kStations, kTyping, kPlaying };

    // Row kinds (ids double as menu tags).
    enum RowTag {
        kTagSearch = 1,
        kTagVoiceSearch,
        kTagTypeSearch,
        kTagHistory,
        kTagQuick,
        kTagSearchHistory,
        kTagFilesHistory,
        kTagPlayed,
        kTagRadio,
        kTagResume,
        kTagExit,
        kTagBack,
        kTagItem,
        kTagFile,
        kTagStation,
    };

    struct Row {
        int tag = kTagBack;
        int index = 0;
        std::string label;
        std::string icon;
    };

    void BuildPanel();
    void UpdateSelection();
    void ShowList(const std::string& title);
    void RebuildRows(const std::vector<Row>& rows, const std::string& title);
    void ActivateRow(int position);
    void Back();
    void GoRoot();
    void StartSearch(const std::string& query);
    void ShowSearchHub(const std::string& title = "");
    void StartVoiceSearch();
    void VoiceSearchTimeout();
    void SaveHistory();
    void LoadHistory();
    void OnSttText(const std::string& text);
    void OpenItem(const MediaItem& item);
    void PlayTrack(const MediaTrack& track);
    void ShowNowPlaying();
    void UpdateNowPlaying();
    void OnPlayerEvent(MusicPlayer::Event event, const MediaTrack& track, int arg);

    // Async fetchers (run on their own task, marshal results back).
    // Result status: kFetchOk / kFetchEmpty ("no results") / kFetchError
    // ("network failed"), so genuine failures are not shown as "no results".
    enum FetchResult {
        kFetchOk = 0,
        kFetchEmpty,
        kFetchError,
    };
    struct FetchItems {
        MediaMode* self;
        std::string query;
    };
    struct FetchFiles {
        MediaMode* self;
        std::string identifier;
        std::string title;
    };
    struct FetchStations {
        MediaMode* self;
        std::string query;
    };
    static void FetchItemsTask(void* arg);
    static void FetchFilesTask(void* arg);
    static void FetchStationsTask(void* arg);

    LcdDisplay* display_ = nullptr;
    AudioCodec* codec_ = nullptr;
    bool open_ = false;
    State state_ = State::kRoot;
    int selected_ = 0;
    std::vector<Row> rows_;

    std::unique_ptr<TextWriter> writer_;
    std::unique_ptr<MusicPlayer> player_;
    MediaTrack last_track_;
    bool has_track_ = false;

    std::vector<MediaItem> items_;
    std::vector<MediaFile> files_;
    std::vector<MediaTrack> stations_;
    // True while waiting for the voice recognizer to transcribe a song query.
    bool voice_search_pending_ = false;
    // Recent search queries (most recent first, capped) - shown in the
    // "Search history" tab.
    std::vector<std::string> history_;
    // Recently played/downloaded tracks (most recent first, capped) - shown
    // in the "Recent files" tab.
    std::vector<MediaTrack> played_;
    // Index into files_ of the currently playing track (for playlist looping).
    int current_file_index_ = -1;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* np_panel_ = nullptr;
    lv_obj_t* np_title_ = nullptr;
    lv_obj_t* np_artist_ = nullptr;
    lv_obj_t* np_state_ = nullptr;
    std::vector<lv_obj_t*> row_objs_;
    // Auto-repeat while up/down are held.
    lv_timer_t* repeat_timer_ = nullptr;
    int repeat_direction_ = 0;
};

#endif
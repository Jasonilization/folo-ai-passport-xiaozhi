#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include "audio/audio_codec.h"
#include "media/media_types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// Streams a full-length track or a radio URL over HTTP, decodes it with the
// system audio decoders, resamples to the codec rate and hands the PCM to the
// audio codec (same path the AI voice uses).
//
// One track at a time; Play() replaces whatever is running. All work happens
// on a dedicated FreeRTOS task so the UI stays responsive.
class MusicPlayer {
public:
    enum class Event {
        kStarted,   // track opened and first PCM decoded
        kProgress,  // ~1 Hz position updates (arg = position_ms)
        kFinished,  // end of stream reached
        kStopped,   // stopped by the user
        kError,     // open/decode failure (arg = esp err code)
    };

    using Callback = std::function<void(Event, const MediaTrack&, int)>;

    explicit MusicPlayer(AudioCodec* codec) : codec_(codec) {}
    ~MusicPlayer();

    void SetCallback(Callback cb) { callback_ = std::move(cb); }

    // Start playing a track (full file or endless radio stream). Replaces any
    // track currently playing.
    void Play(const MediaTrack& track);
    // Stop playback and release the codec.
    void Stop();
    // Pause keeps the HTTP connection and decoder state alive.
    void SetPaused(bool paused);
    void TogglePaused();

    bool IsPlaying() const { return playing_; }
    bool IsPaused() const { return paused_; }
    const MediaTrack& current_track() const { return current_; }

private:
    static void TaskEntry(void* arg);
    void Run();
    void RunImpl();
    void Cleanup();
    void Emit(Event event, int arg = 0);
    bool OutputPcm(const uint8_t* data, uint32_t bytes, int sample_rate,
                   int channels, int bits);
    void CloseResampler();

    AudioCodec* codec_ = nullptr;
    Callback callback_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> playing_{false};
    MediaTrack current_;

    // Task + HTTP handle are owned by the playback task; guarded by stop_.
    void* task_ = nullptr;
    void* http_ = nullptr;
    void* decoder_ = nullptr;
    void* resampler_ = nullptr;
    int resampler_channels_ = 0;
    std::atomic<bool> saw_disconnect_{false};
};

#endif
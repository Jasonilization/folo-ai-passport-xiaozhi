#include "music_player.h"

#include "application.h"

#include <decoder/esp_audio_dec.h>
#include <decoder/esp_audio_dec_default.h>
#include <esp_ae_rate_cvt.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <netdb.h>

#include <atomic>
#include <cstring>
#include <vector>

#define TAG "MusicPlayer"

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                                    \
    (esp_ae_rate_cvt_cfg_t) {                                                            \
        .src_rate = (uint32_t)(_src_rate), .dest_rate = (uint32_t)(_dest_rate),          \
        .channel = (uint8_t)(_channel), .bits_per_sample = ESP_AUDIO_BIT16,              \
        .complexity = 2, .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                   \
    }

namespace {

constexpr size_t kInBufSize = 32 * 1024;
constexpr uint32_t kOutBufSize = 16 * 1024;

esp_audio_type_t TypeFromUrl(const std::string& url) {
    std::string lower;
    for (char c : url) {
        lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    }
    size_t q = lower.find('?');
    if (q != std::string::npos) {
        lower = lower.substr(0, q);
    }
    if (lower.find(".mp3") != std::string::npos) return ESP_AUDIO_TYPE_MP3;
    if (lower.find(".ogg") != std::string::npos) return ESP_AUDIO_TYPE_VORBIS;
    if (lower.find(".oga") != std::string::npos) return ESP_AUDIO_TYPE_VORBIS;
    if (lower.find(".flac") != std::string::npos) return ESP_AUDIO_TYPE_FLAC;
    if (lower.find(".aac") != std::string::npos) return ESP_AUDIO_TYPE_AAC;
    if (lower.find(".m4a") != std::string::npos) return ESP_AUDIO_TYPE_AAC;
    if (lower.find(".amr") != std::string::npos) return ESP_AUDIO_TYPE_AMRNB;
    return ESP_AUDIO_TYPE_UNSUPPORT;
}

esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ERROR || evt->event_id == HTTP_EVENT_DISCONNECTED ||
        evt->event_id == HTTP_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "http event %d", evt->event_id);
        // Peer already closed: remember it so Cleanup() does not call
        // esp_http_client_close() on a dead connection (double pbuf free).
        if (evt->user_data != nullptr) {
            *static_cast<std::atomic<bool>*>(evt->user_data) = true;
        }
    }
    return ESP_OK;
}

bool RegisterDecodersOnce() {
    static bool done = false;
    if (!done) {
        esp_audio_err_t ret = esp_audio_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "register_default failed: %d", ret);
            return false;
        }
        done = true;
    }
    return true;
}

}  // namespace

MusicPlayer::~MusicPlayer() {
    Stop();
}

void MusicPlayer::Play(const MediaTrack& track) {
    Stop();
    current_ = track;
    stop_ = false;
    paused_ = false;
    if (task_ != nullptr) {
        // Give the previous task a moment to exit; Stop() already waited.
        vTaskDelete(static_cast<TaskHandle_t>(task_));
        task_ = nullptr;
    }
    if (xTaskCreate(TaskEntry, "music_player", 10240, this, 4,
                    reinterpret_cast<TaskHandle_t*>(&task_)) != pdPASS) {
        ESP_LOGE(TAG, "failed to start playback task");
        Emit(Event::kError, ESP_AUDIO_ERR_FAIL);
        return;
    }
    playing_ = true;
}

void MusicPlayer::Stop() {
    if (!playing_ && task_ == nullptr) {
        return;
    }
    stop_ = true;
    // Do NOT close http_ from here: the playback task may be blocked inside
    // esp_http_client_read on it. It has a short read timeout and checks
    // stop_ every loop, so it exits on its own.
    // Wait for the task to exit on its own (read timeout is 3 s, so this
    // converges). NEVER vTaskDelete it here: killing a task mid-DNS/TLS leaves
    // lwIP callbacks on freed memory and crashes the core.
    int wait = 0;
    while (task_ != nullptr && wait < 600) {
        vTaskDelay(pdMS_TO_TICKS(20));
        wait += 20;
    }
    if (task_ != nullptr) {
        ESP_LOGW(TAG, "playback task still running; leaving it (leak > crash)");
    }
    stop_ = false;
    paused_ = false;
    playing_ = false;
    Emit(Event::kStopped, 0);
}

void MusicPlayer::SetPaused(bool paused) {
    paused_ = paused;
}

void MusicPlayer::TogglePaused() {
    paused_ = !paused_;
}

void MusicPlayer::TaskEntry(void* arg) {
    auto* self = static_cast<MusicPlayer*>(arg);
    self->Run();
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

void MusicPlayer::Run() {
    try {
        RunImpl();
    } catch (...) {
        ESP_LOGE(TAG, "playback task threw (out of memory?)");
        Cleanup();
        Emit(Event::kError, ESP_AUDIO_ERR_MEM_LACK);
    }
}

void MusicPlayer::RunImpl() {
    // A TLS handshake needs ~25KB+ free; below that it corrupts lwIP/heap on
    // the C3, so refuse cleanly instead of crashing the device.
    if (esp_get_free_heap_size() < 45000) {
        ESP_LOGW(TAG, "not enough heap for playback (%u), refusing",
                 (unsigned)esp_get_free_heap_size());
        Emit(Event::kError, ESP_AUDIO_ERR_MEM_LACK);
        return;
    }
    if (codec_ == nullptr) {
        Emit(Event::kError, ESP_AUDIO_ERR_FAIL);
        return;
    }

    esp_audio_type_t type = TypeFromUrl(current_.url);
    if (type == ESP_AUDIO_TYPE_UNSUPPORT || !RegisterDecodersOnce()) {
        ESP_LOGE(TAG, "unsupported audio url: %s", current_.url.c_str());
        Emit(Event::kError, ESP_AUDIO_ERR_NOT_SUPPORT);
        return;
    }

    // archive.org /download/ URLs answer 302 to a CDN mirror; manual open/read
    // does not follow redirects (only esp_http_client_perform does), so follow
    // them here, up to 5 hops.
    std::string url = current_.url;
    esp_http_client_handle_t client = nullptr;
    int status = 0;
    // DNS on this link intermittently fails (getaddrinfo 202); warm the lwIP
    // resolver cache with retries before each connect attempt, and retry the
    // whole open a few times.
    auto warm_dns = [](const std::string& u) {
        std::string host = u;
        size_t scheme = host.find("://");
        if (scheme != std::string::npos) host = host.substr(scheme + 3);
        size_t slash = host.find('/');
        if (slash != std::string::npos) host = host.substr(0, slash);
        for (int i = 0; i < 6; i++) {
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            struct addrinfo* res = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0) {
                if (res != nullptr) freeaddrinfo(res);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    };
    for (int attempt = 0; attempt < 3 && (client == nullptr || status != 200); attempt++) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(3000));
        warm_dns(url);
        for (int hop = 0; hop < 5; hop++) {
            esp_http_client_config_t config = {};
            config.url = url.c_str();
            config.timeout_ms = 15000;
            config.event_handler = HttpEventHandler;
            config.buffer_size = 4096;
            config.user_data = &saw_disconnect_;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            client = esp_http_client_init(&config);
            // fetch_headers returns content length (-1/0 for chunked radio
            // streams is NOT an error, so don't check it).
            if (client == nullptr || esp_http_client_open(client, 0) != ESP_OK) {
                ESP_LOGW(TAG, "http open failed for %s", url.c_str());
                if (client != nullptr) {
                    esp_http_client_cleanup(client);
                    client = nullptr;
                }
                break;  // next attempt
            }
            esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);
            if (status >= 300 && status < 400) {
                char* loc = nullptr;
                // Response headers need SAVE_RESPONSE_HEADERS; get_header()
                // reads request headers and would never find Location.
                if (esp_http_client_get_response_header(client, "Location", &loc) == ESP_OK &&
                    loc != nullptr) {
                    // NOTE: loc points into the client's internal header
                    // storage - must NOT be freed (that corrupted the heap).
                    std::string next = loc;
                    if (next.rfind("http", 0) != 0) {
                        size_t scheme = url.find("://");
                        size_t slash = url.find('/', scheme + 3);
                        std::string base =
                            (slash == std::string::npos) ? url : url.substr(0, slash);
                        next = base + (next.empty() || next[0] != '/' ? "/" : "") + next;
                    }
                    url = std::move(next);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    client = nullptr;
                    continue;  // next hop
                }
            }
            break;  // got a non-redirect response
        }
    }
    if (client == nullptr || status != 200) {
        ESP_LOGE(TAG, "http status %d for %s", status, url.c_str());
        if (client != nullptr) {
            esp_http_client_cleanup(client);
        }
        Emit(Event::kError, ESP_AUDIO_ERR_FAIL);
        return;
    }
    http_ = client;
    ESP_LOGI(TAG, "stage: connected %s status=%d", url.c_str(), status);

    esp_audio_dec_cfg_t dec_cfg = {};
    dec_cfg.type = type;
    dec_cfg.cfg = nullptr;
    dec_cfg.cfg_sz = 0;
    ESP_LOGI(TAG, "stage: opening decoder type=%d", (int)type);
    if (esp_audio_dec_open(&dec_cfg, &decoder_) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed");
        Cleanup();
        Emit(Event::kError, ESP_AUDIO_ERR_FAIL);
        return;
    }

    codec_->EnableOutput(true);

    // malloc'd buffers only: C++ exceptions are disabled in this build, so a
    // throwing vector allocation under heap pressure would abort the device.
    uint8_t* in = static_cast<uint8_t*>(malloc(kInBufSize));
    uint8_t* out = static_cast<uint8_t*>(malloc(kOutBufSize));
    if (in == nullptr || out == nullptr) {
        free(in);
        free(out);
        Cleanup();
        Emit(Event::kError, ESP_AUDIO_ERR_MEM_LACK);
        return;
    }
    size_t in_len = 0, in_pos = 0;
    bool eof = false;
    int64_t content_length = esp_http_client_get_content_length(client);
    int64_t total_read = 0;
    int64_t last_emit = -2000;
    int64_t last_read = 0;
    bool started = false;
    esp_err_t last_ok = ESP_OK;

    while (!stop_) {
        if (paused_) {
            // Just stop feeding the decoder; keep the HTTP connection and the
            // codec untouched so resume is instant and crash-free.
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (in_pos > 0) {
            memmove(in, in + in_pos, in_len - in_pos);
            in_len -= in_pos;
            in_pos = 0;
        }
        if (in_len < kInBufSize / 2 && !eof) {
            int r = esp_http_client_read(client, reinterpret_cast<char*>(in + in_len),
                                         kInBufSize - in_len);
            if (r > 0) {
                in_len += r;
                total_read += r;
            } else if (r == 0) {
                eof = true;
            } else {
                ESP_LOGW(TAG, "http read error %d", r);
                break;
            }
        }
        if (in_len == 0 && eof) {
            break;
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = in + in_pos;
        raw.len = (uint32_t)(in_len - in_pos);
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = out;
        frame.len = kOutBufSize;
        frame.decoded_size = 0;

        esp_audio_err_t err = esp_audio_dec_process(decoder_, &raw, &frame);
        if (frame.decoded_size > 0 && total_read < 1) {
            ESP_LOGI(TAG, "stage: first decoded frame %u bytes", frame.decoded_size);
        }
        if (frame.decoded_size > 0) {
            esp_audio_dec_info_t info = {};
            if (esp_audio_dec_get_info(decoder_, &info) == ESP_AUDIO_ERR_OK) {
                OutputPcm(out, frame.decoded_size, (int)info.sample_rate,
                          info.channel ? info.channel : 1, info.bits_per_sample);
            }
            if (!started) {
                started = true;
                Emit(Event::kStarted, 0);
            }
        }
        if (raw.consumed > 0) {
            in_pos += raw.consumed;
        }

        if (err == ESP_AUDIO_ERR_DATA_LACK) {
            if (eof) {
                break;
            }
            // Decoder wants more data: KEEP the buffered input (dropping it
            // starves the decoder forever). Only if the buffer is completely
            // full and nothing was consumed do we drop one byte to force
            // progress.
            if (raw.consumed == 0 && frame.decoded_size == 0 &&
                in_len - in_pos >= kInBufSize) {
                in_pos += 1;
            }
            continue;
        }
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            // Output buffer too small: drop the frame rather than growing
            // the buffer (allocation may fail under heap pressure).
            in_pos = in_len;
            continue;
        }
        if (err == ESP_AUDIO_ERR_FAIL || err == ESP_AUDIO_ERR_NOT_SUPPORT) {
            // Desync: drop one byte so the decoder can resync on the next
            // frame syncword (dropping everything would stall forever).
            in_pos += 1;
            if (last_ok == ESP_OK && err == ESP_AUDIO_ERR_NOT_SUPPORT) {
                last_ok = err;
            }
            continue;
        }
        if (content_length > 0 && !current_.is_stream) {
            // Rough position from bytes consumed, assuming ~256 kbps audio.
            int64_t position_ms = total_read * 8 / 256;
            if (position_ms - last_emit > 1000) {
                last_emit = position_ms;
                int64_t kbps = (total_read - last_read) / 1024;
                last_read = total_read;
                ESP_LOGI(TAG, "stream %lld KB total, ~%lld KB/s", (long long)(total_read / 1024),
                         (long long)kbps);
                Emit(Event::kProgress, (int)position_ms);
            }
        }
    }

    free(in);
    free(out);
    bool was_stopped = stop_;
    Cleanup();
    if (was_stopped) {
        Emit(Event::kStopped, 0);
    } else {
        Emit(Event::kFinished, 0);
    }
}

void MusicPlayer::Cleanup() {
    CloseResampler();
    if (decoder_ != nullptr) {
        esp_audio_dec_close(decoder_);
        decoder_ = nullptr;
    }
    if (http_ != nullptr) {
        auto client = static_cast<esp_http_client_handle_t>(http_);
        if (!saw_disconnect_) {
            esp_http_client_close(client);
        }
        esp_http_client_cleanup(client);
        http_ = nullptr;
    }
    saw_disconnect_ = false;
    playing_ = false;
}

void MusicPlayer::CloseResampler() {
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(static_cast<esp_ae_rate_cvt_handle_t>(resampler_));
        resampler_ = nullptr;
    }
}

bool MusicPlayer::OutputPcm(const uint8_t* data, uint32_t bytes, int sample_rate,
                            int channels, int bits) {
    if (bits != 16) {
        return false;
    }
    uint32_t samples = bytes / sizeof(int16_t);
    if (samples == 0) {
        return true;
    }
    // The codec path copies into std::vectors; with exceptions enabled a failed
    // allocation is catchable, but still drop the frame if heap is very low.
    if (esp_get_free_heap_size() < samples * 2 + 8192) {
        return false;
    }
    const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
    int dest_rate = codec_->output_sample_rate();
    int dest_channels = codec_->output_channels();

    // The Passport's ES8311 output is mono: downmix stereo first.
    std::vector<int16_t> mono;
    if (channels == 2 && dest_channels == 1) {
        uint32_t frames = samples / 2;
        mono.resize(frames);
        for (uint32_t i = 0; i < frames; i++) {
            mono[i] = static_cast<int16_t>((pcm[2 * i] + pcm[2 * i + 1]) / 2);
        }
        pcm = mono.data();
        samples = frames;
        channels = 1;
    }

    if (sample_rate == dest_rate && channels == dest_channels) {
        std::vector<int16_t> out(pcm, pcm + samples);
        codec_->OutputData(out);
        return true;
    }

    if (resampler_ == nullptr || resampler_channels_ != channels) {
        CloseResampler();
        auto cfg = RATE_CVT_CFG(sample_rate, dest_rate, channels);
        if (esp_ae_rate_cvt_open(&cfg, reinterpret_cast<esp_ae_rate_cvt_handle_t*>(&resampler_)) !=
            ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "resampler open failed");
            return false;
        }
        resampler_channels_ = channels;
    }

    uint32_t frames = samples / channels;
    uint32_t max_out = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(
            static_cast<esp_ae_rate_cvt_handle_t>(resampler_), frames, &max_out) !=
        ESP_AE_ERR_OK) {
        return false;
    }
    std::vector<int16_t> converted(max_out * channels);
    esp_ae_sample_t in = (esp_ae_sample_t)pcm;
    esp_ae_sample_t out_buf = (esp_ae_sample_t)converted.data();
    uint32_t out_frames = max_out;
    if (esp_ae_rate_cvt_process(static_cast<esp_ae_rate_cvt_handle_t>(resampler_), in, frames,
                                out_buf, &out_frames) != ESP_AE_ERR_OK) {
        return false;
    }
    if (out_frames == 0) {
        return true;
    }
    converted.resize(out_frames * channels);
    codec_->OutputData(converted);
    return true;
}

void MusicPlayer::Emit(Event event, int arg) {
    if (callback_) {
        // Marshal to the main loop so the callback may touch LVGL safely.
        auto* self = this;
        MediaTrack track = current_;
        Application::GetInstance().Schedule([self, event, track, arg]() {
            if (self->callback_) {
                self->callback_(event, track, arg);
            }
        });
    }
}
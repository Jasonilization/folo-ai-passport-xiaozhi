#include "wifi_board.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "main_menu.h"
#include "media/media_mode.h"
#include "media/archive_client.h"
#include "media/radio_client.h"
#include "media/music_player.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"
#include "cw2017_battery_monitor.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <button_adc.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_netif.h>
#include <cstring>
#include <nvs_flash.h>
#include <lvgl.h>
#include <esp_console.h>
#include <atomic>
#include <vector>

#define TAG "AiPassport"

class AiPassportBoard;
static AiPassportBoard* g_board_for_console = nullptr;
static void StartMediaConsole();
// Boot-time self test
// Boot-time self test: exercises the exact media path the UI uses (archive.org
// search, folder listing, HTTP streaming + decode of a real track, radio
// directory) and logs it to the serial monitor. Defined after the class.
static void MediaSelfTestTask(void* arg);

// HTTPS certificate validation needs a sane system clock; the stock firmware
// only gets time from the OTA server response, so sync via SNTP once WiFi is
// up. lwIP retries until the interface is ready.
static void SntpInitTask(void* arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_sntp_config_t opts = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    opts.sync_cb = nullptr;
    esp_netif_sntp_init(&opts);
    ESP_LOGI(TAG, "SNTP sync started (pool.ntp.org)");

    // Pin public DNS servers on the STA interface: the DHCP-provided resolver
    // intermittently fails (getaddrinfo error 202), which killed all HTTPS
    // fetches. Retry until the interface is up.
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta != nullptr) {
        for (int i = 0; i < 30; i++) {
            esp_netif_dns_info_t dns;
            memset(&dns, 0, sizeof(dns));
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
            if (esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
                memset(&dns, 0, sizeof(dns));
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
                esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns);
                ESP_LOGI(TAG, "DNS pinned to 8.8.8.8 / 1.1.1.1");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    vTaskDelete(nullptr);
}

// Physical keys share one ADC pin through a resistor ladder (see config.h).
enum {
    kAdcButtonUp = 0,
    kAdcButtonDown,
    kAdcButtonOk,
    kAdcButtonNum,
};

class AiPassportBoard : public WifiBoard {
private:
    // Main menu item ids. Features that are not built yet stay in the list and
    // report "not installed" until their mode lands.
    enum MenuItemId {
        kMenuChat = 1,     // back to the conversation
        kMenuHistory,
        kMenuMicViz,
        kMenuMusic,    // archive.org music + radio player with on-device search
        kMenuWifi,     // add another network (multi-network fallback)
        kMenuRotate,   // 90-degree landscape toggle
        kMenuDoom,
        kMenuAbout,
        kMenuExit,
    };

    i2c_master_bus_handle_t codec_i2c_bus_;
    Button* adc_button_[kAdcButtonNum];
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    LcdDisplay* display_;
    Cw2017BatteryMonitor* battery_;
    // While true, up/down scroll the chat history instead of changing volume.
    bool history_mode_ = false;
    // The main menu overlay; created once the display exists.
    std::unique_ptr<MainMenu> menu_;
    // Music/radio browser + player; closed unless opened from the menu.
    std::unique_ptr<MediaMode> media_;
    bool mic_viz_enabled_ = true;
    // 90-degree landscape rotation, persisted in NVS.
    bool landscape_ = false;

    // Vertical scroll step for history paging, in pixels.
    static constexpr int kHistoryPageStep = 80;

    bool IsMenuOpen() const {
        return menu_ != nullptr && menu_->IsOpen();
    }

    bool IsMediaOpen() const {
        return media_ != nullptr && media_->IsOpen();
    }

    void ShowToast(const char* text) {
        if (display_ != nullptr) {
            display_->ShowNotification(text);
        }
    }

    std::vector<MainMenu::Item> BuildMenuItems() {
        std::vector<MainMenu::Item> items;
        items.push_back({kMenuChat, "Chat", MATERIAL_SYMBOLS_CHAT_BUBBLE});
        items.push_back({kMenuHistory, "History", MATERIAL_SYMBOLS_MEMORY});
        items.push_back({kMenuMicViz, mic_viz_enabled_ ? "Mic Bars: On" : "Mic Bars: Off",
                         MATERIAL_SYMBOLS_MIC});
        items.push_back({kMenuMusic, "Music / Radio", MATERIAL_SYMBOLS_MUSIC_NOTE});
        items.push_back({kMenuWifi, "Add WiFi Network", MATERIAL_SYMBOLS_WIFI});
        items.push_back({kMenuRotate, landscape_ ? "Landscape: On" : "Landscape: Off",
                         MATERIAL_SYMBOLS_NAVIGATION});
        items.push_back({kMenuDoom, "Doom", MATERIAL_SYMBOLS_SPORTS_ESPORTS});
        items.push_back({kMenuAbout, "About", MATERIAL_SYMBOLS_INFO});
        items.push_back({kMenuExit, "Exit", MATERIAL_SYMBOLS_CLOSE});
        return items;
    }

    void HandleMenuActivate(int id) {
        switch (id) {
            case kMenuChat:
                // Return to the conversation: leave history mode and any
                // open media mode, then close the menu.
                history_mode_ = false;
                if (media_ != nullptr && media_->IsOpen()) {
                    media_->Close();
                }
                break;
            case kMenuHistory:
                ToggleHistoryMode();
                break;
            case kMenuMicViz: {
                mic_viz_enabled_ = !mic_viz_enabled_;
                if (display_ != nullptr) {
                    display_->SetMicVisualizerEnabled(mic_viz_enabled_);
                }
                ShowToast(mic_viz_enabled_ ? "Mic bars on" : "Mic bars off");
                break;
            }
            case kMenuMusic:
                if (media_ == nullptr) {
                    media_ = std::make_unique<MediaMode>(display_, GetAudioCodec());
                }
                media_->Open();
                break;
            case kMenuRotate:
                ToggleLandscape();
                break;
            case kMenuWifi:
                // Re-enter the provisioning portal so another network can be
                // added; every saved network is tried (RSSI-ordered) at boot.
                ShowToast("Join the setup hotspot");
                EnterWifiConfigMode();
                break;
            case kMenuDoom:
                ShowToast("Doom not installed yet");
                break;
            case kMenuAbout: {
                char info[96];
                snprintf(info, sizeof(info), "xiaozhi-ai 2.5.0 | SRAM %uKB",
                         (unsigned)(esp_get_free_heap_size() / 1024));
                ShowToast(info);
                break;
            }
            case kMenuExit:
            default:
                break;
        }
        if (menu_ != nullptr) {
            menu_->Close();
        }
    }

    void LoadLandscape() {
        nvs_handle_t h;
        if (nvs_open("passport", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 0;
            nvs_get_u8(h, "landscape", &v);
            landscape_ = v != 0;
            nvs_close(h);
        }
    }

    void ApplyLandscape() {
        // Hardware rotation is applied at boot (panel swap_xy), so a toggle
        // takes effect after reboot.
    }

    void ToggleLandscape() {
        landscape_ = !landscape_;
        nvs_handle_t h;
        if (nvs_open("passport", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "landscape", landscape_ ? 1 : 0);
            nvs_commit(h);
            nvs_close(h);
        }
        ShowToast(landscape_ ? "Landscape on - reboot to apply" : "Portrait - reboot to apply");
    }

    // The Passport UI is always dark; no light mode.
    void ForceDarkTheme() {
        if (display_ == nullptr) {
            return;
        }
        auto& theme_manager = LvglThemeManager::GetInstance();
        auto* dark = static_cast<LvglTheme*>(theme_manager.GetTheme("dark"));
        if (dark != nullptr && display_->GetTheme() != static_cast<Theme*>(dark)) {
            display_->SetTheme(static_cast<Theme*>(dark));
        }
    }

    void ToggleHistoryMode() {
        history_mode_ = !history_mode_;
        if (display_ != nullptr) {
            display_->ShowNotification(history_mode_ ? "History - up/down scroll, OK exit"
                                                     : "");
        }
    }

    void ScrollHistory(int step) {
        if (history_mode_ && display_ != nullptr) {
            display_->ScrollChatMessages(step);
        }
    }

    void OnOkClick() {
        if (history_mode_) {
            ToggleHistoryMode();
            return;
        }
        ToggleChat();
    }

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // CW2017 fuel gauge is optional; a missing chip just disables battery UI.
        battery_ = new Cw2017BatteryMonitor(codec_i2c_bus_, BATTERY_CW2017_ADDR);
        battery_->Initialize();
    }

    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        auto volume = codec->output_volume() + delta;
        if (volume > 100) {
            volume = 100;
        }
        if (volume < 0) {
            volume = 0;
        }
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void ToggleChat() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    }

    void InitializeButtons() {
        for (int i = 0; i < kAdcButtonNum; i++) {
            adc_button_[i] = nullptr;
        }

        // One ADC1 unit shared by all three ladder keys. AdcButton reuses the
        // handle when adc_config.adc_handle is non-null, so the same physical
        // pin can decode several keys without "adc1 is already in use".
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle_));

        button_adc_config_t adc_cfg = {};
        adc_cfg.adc_handle = &adc_handle_;
        adc_cfg.unit_id = ADC_UNIT_1;
        adc_cfg.adc_channel = ADC_CHANNEL_0;  // GPIO0

        adc_cfg.button_index = kAdcButtonUp;      // UP:   ~0 mV
        adc_cfg.min = BSP_ADC_BUTTON_UP_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_UP_MAX;
        adc_button_[kAdcButtonUp] = new AdcButton(adc_cfg);

        adc_cfg.button_index = kAdcButtonDown;    // DOWN: ~300 mV
        adc_cfg.min = BSP_ADC_BUTTON_DOWN_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_DOWN_MAX;
        adc_button_[kAdcButtonDown] = new AdcButton(adc_cfg);

        adc_cfg.button_index = kAdcButtonOk;      // OK:   ~595 mV
        adc_cfg.min = BSP_ADC_BUTTON_OK_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_OK_MAX;
        adc_button_[kAdcButtonOk] = new AdcButton(adc_cfg);

        // Button callbacks run on the button task; schedule all UI/audio
        // work onto the main task so LVGL and codec access stay on one thread.
        // Gesture model:
        //   home:    up/down = volume, OK click = talk, OK double = history,
        //            OK hold = main menu
        //   menu:    up/down = move selection, OK click = activate, OK hold = close
        //   media:   up/down = navigate / volume, OK click = activate / pause,
        //            OK double = back / stop, OK hold = exit
        auto up = adc_button_[kAdcButtonUp];
        up->OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsMediaOpen()) {
                    media_->OnUp();
                } else if (IsMenuOpen()) {
                    menu_->MoveSelection(-1);
                } else if (history_mode_) {
                    ScrollHistory(-kHistoryPageStep);
                } else {
                    ChangeVolume(10);
                }
            });
        });
        up->OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsMediaOpen()) {
                    media_->OnUpHoldStart();
                    return;
                }
                if (IsMenuOpen()) {
                    menu_->MoveHoldStart(-1);
                    return;
                }
                if (history_mode_) {
                    ScrollHistory(-kHistoryPageStep);
                    return;
                }
                GetAudioCodec()->SetOutputVolume(100);
                GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
            });
        });
        up->OnPressUp([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsMediaOpen()) {
                    media_->OnHoldStop();
                } else if (IsMenuOpen()) {
                    menu_->HoldStop();
                }
            });
        });

        auto down = adc_button_[kAdcButtonDown];
        down->OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsMediaOpen()) {
                    media_->OnDown();
                } else if (IsMenuOpen()) {
                    menu_->MoveSelection(1);
                } else if (history_mode_) {
                    ScrollHistory(kHistoryPageStep);
                } else {
                    ChangeVolume(-10);
                }
            });
        });
        down->OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsMediaOpen()) {
                    media_->OnDownHoldStart();
                    return;
                }
                if (IsMenuOpen()) {
                    menu_->MoveHoldStart(1);
                    return;
                }
                if (history_mode_) {
                    ScrollHistory(kHistoryPageStep);
                    return;
                }
                GetAudioCodec()->SetOutputVolume(0);
                GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            });
        });
        down->OnPressUp([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsMediaOpen()) {
                    media_->OnHoldStop();
                } else if (IsMenuOpen()) {
                    menu_->HoldStop();
                }
            });
        });

        auto ok = adc_button_[kAdcButtonOk];
        ok->OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsMediaOpen()) {
                    media_->OnOkClick();
                } else if (IsMenuOpen()) {
                    menu_->Activate();
                } else {
                    OnOkClick();
                }
            });
        });
        ok->OnDoubleClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsMediaOpen()) {
                    media_->OnOkDouble();
                } else {
                    ToggleHistoryMode();
                }
            });
        });
        ok->OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsMediaOpen()) {
                    media_->OnOkHold();
                } else if (IsMenuOpen()) {
                    history_mode_ = false;
                    menu_->Close();
                } else {
                    history_mode_ = false;
                    menu_->Open(BuildMenuItems());
                }
            });
        });
        // OK hold opens the main menu (theme toggle moved into the menu);
        // OK double-click scrolls back through the conversation history.
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;  // -1 -> software reset
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);

        // Panel-specific power/porch/gamma sequence for the ST7789P3 module
        // used on the Passport (copied from the original badge firmware).
        static const struct {
            uint8_t command;
            uint8_t data[16];
            uint8_t data_length;
            uint16_t delay_ms;
        } kSt7789P3InitCommands[] = {
            {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},
            {0xB7, {0x35}, 1, 0},
            {0xBB, {0x21}, 1, 0},
            {0xC0, {0x2C}, 1, 0},
            {0xC2, {0x01}, 1, 0},
            {0xC3, {0x0B}, 1, 0},
            {0xC4, {0x20}, 1, 0},
            {0xC6, {0x0F}, 1, 0},
            {0xD0, {0xA7, 0xA1}, 2, 0},
            {0xD0, {0xA4, 0xA1}, 2, 0},
            {0xD6, {0xA1}, 1, 0},
            {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
                    0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},
            {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
                    0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},
        };
        for (const auto& cmd : kSt7789P3InitCommands) {
            esp_lcd_panel_io_tx_param(panel_io, cmd.command, cmd.data, cmd.data_length);
            if (cmd.delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(cmd.delay_ms));
            }
        }

        // Landscape = hardware 90 deg rotation of the panel (swap_xy), with the
        // logical resolution swapped accordingly. Applied at boot from NVS so
        // LVGL is created with the right hres/vres (no software rotation).
        const bool swap_xy = DISPLAY_SWAP_XY != landscape_;
        // Rotating 90 deg also needs both mirrors flipped, otherwise the
        // landscape image comes out upside down.
        // swap_xy rotates 90 deg; flipping only mirror_x corrects the upside-
        // down orientation (flipping both would add a 180 deg turn).
        const bool mirror_x = DISPLAY_MIRROR_X != landscape_;
        const bool mirror_y = DISPLAY_MIRROR_Y;
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_set_gap(panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
        esp_lcd_panel_swap_xy(panel, swap_xy);
        esp_lcd_panel_mirror(panel, mirror_x, mirror_y);
        esp_lcd_panel_disp_on_off(panel, true);

        const int width = swap_xy ? DISPLAY_HEIGHT : DISPLAY_WIDTH;
        const int height = swap_xy ? DISPLAY_WIDTH : DISPLAY_HEIGHT;
        display_ = new SpiLcdDisplay(panel_io, panel,
                                     width, height,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     mirror_x, mirror_y, swap_xy);
    }

public:
    AiPassportBoard() : display_(nullptr), battery_(nullptr) {
        LoadLandscape();  // before display init: picks hardware rotation
        InitializeCodecI2c();
        InitializeSpi();
        InitializeDisplay();
        // The display must exist before the menu can attach to it.
        menu_ = std::make_unique<MainMenu>(display_);
        menu_->SetActivateCallback([this](int id) { HandleMenuActivate(id); });
        // Media mode is created lazily on first use: building it here would
        // construct the ES8311 codec much earlier in boot than the app does.

        // The boot self-test is disabled: its concurrent TLS/streaming at 12 s
        // was the heap-corruption crash correlator (crashes could wipe NVS,
        // which then dropped the device into WiFi config mode on next boot).
        // Keep the function for manual runs via a serial console if needed.
        // xTaskCreate(MediaSelfTestTask, "media_selftest", 10240, this, 3, nullptr);
        xTaskCreate(SntpInitTask, "sntp_init", 3072, nullptr, 3, nullptr);
        g_board_for_console = this;
        StartMediaConsole();
        // Force dark theme + apply saved rotation once LVGL is up.
        LoadLandscape();
        Application::GetInstance().Schedule([this]() {
            ForceDarkTheme();
            ApplyLandscape();
        });
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    // Serial-console media test hooks (drives the real UI code paths).
    void DebugMedia(const std::string& sub, const std::string& a, const std::string& b) {
        Application::GetInstance().Schedule([this, sub, a, b]() {
            if (media_ == nullptr) {
                media_ = std::make_unique<MediaMode>(display_, GetAudioCodec());
            }
            // Free the audio channel's heap BEFORE fetching: schedule the
            // close, then wait for it to run (it is itself scheduled on the
            // main task, which is where this lambda runs).
            Application::GetInstance().CloseAudioChannel();
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP_LOGI(TAG, "mt %s: heap before fetch=%u", sub.c_str(),
                     (unsigned)esp_get_free_heap_size());
            if (sub == "search") {
                media_->DebugSearch(a);
            } else if (sub == "list") {
                media_->DebugList(a);
            } else if (sub == "play") {
                media_->DebugPlay(a, b.empty() ? 6 : atoi(b.c_str()));
            } else if (sub == "radio") {
                media_->Open();
                media_->DebugSearch("");  // placeholder no-op; radio via UI row
            }
        });
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!battery_ || !battery_->IsPresent()) {
            return false;
        }
        int soc = battery_->GetBatteryLevel();
        if (soc < 0) {
            return false;
        }
        level = soc;
        // CW2017 reports no charge state and the Passport has no charge-detect
        // GPIO, so report a plain (discharging) reading.
        charging = false;
        discharging = true;
        return true;
    }
};

static void MediaSelfTestTask(void* arg) {
    auto* board = static_cast<AiPassportBoard*>(arg);
    // Run while the heap is still high: once the MQTT audio channel opens
    // (~16s+) free heap drops to ~30KB and TLS handshakes start failing.
    vTaskDelay(pdMS_TO_TICKS(12000));
    ESP_LOGI("AiPassport", "self-test: free heap %u", (unsigned)esp_get_free_heap_size());

    ArchiveClient archive;

    // Song search: a song title and an artist (multi-language titles included).
    for (const char* q : {"asgore", "kessoku band", "undertale megalovania"}) {
        std::vector<MediaItem> items;
        bool ok = archive.Search(q, 5, items);
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(6000));
            ok = archive.Search(q, 5, items);
        }
        ESP_LOGI("AiPassport", "self-test: search \"%s\" -> %s (%u items)", q,
                 ok ? "ok" : "FAILED", (unsigned)items.size());
        for (const auto& item : items) {
            ESP_LOGI("AiPassport", "  [%s] %s", item.identifier.c_str(),
                     item.title.c_str());
        }
        if (ok && !items.empty() && strcmp(q, "asgore") == 0) {
            // Folder browse: list the audio files of the first hit.
            std::vector<MediaFile> files;
            ESP_LOGI("AiPassport", "self-test: listing %s ...",
                     items[0].identifier.c_str());
            bool fok = archive.ListFiles(items[0].identifier, files);
            if (!fok) {
                vTaskDelay(pdMS_TO_TICKS(4000));
                fok = archive.ListFiles(items[0].identifier, files);
            }
            ESP_LOGI("AiPassport", "self-test: files %s (%u entries), heap %u",
                     fok ? "ok" : "FAILED", (unsigned)files.size(),
                     (unsigned)esp_get_free_heap_size());
            // Playback: stream + decode the first audio file for ~8 s.
            if (fok && !files.empty()) {
                MediaTrack track;
                track.title = files[0].name;
                track.url = files[0].url;
                ESP_LOGI("AiPassport", "self-test: playing %s", track.url.c_str());
                MusicPlayer player(board->GetAudioCodec());
                std::atomic<int> events{0};
                player.SetCallback([&events](MusicPlayer::Event ev, const MediaTrack& t,
                                             int a) {
                    events++;
                    ESP_LOGI("AiPassport", "  player event %d arg=%d (%s)", (int)ev, a,
                             t.title.c_str());
                });
                player.Play(track);
                vTaskDelay(pdMS_TO_TICKS(8000));
                ESP_LOGI("AiPassport", "self-test: after 8s playing=%d events=%d heap=%u",
                         (int)player.IsPlaying(), (int)events.load(),
                         (unsigned)esp_get_free_heap_size());
                player.Stop();
            }
        }
    }

    // Radio directory.
    std::vector<MediaTrack> stations;
    RadioClient radio;
    bool rok = radio.Search("lofi", 5, stations);
    ESP_LOGI("AiPassport", "self-test: radio -> %s (%u stations)", rok ? "ok" : "FAILED",
             (unsigned)stations.size());
    for (const auto& st : stations) {
        ESP_LOGI("AiPassport", "  station: %s -> %s", st.title.c_str(), st.url.c_str());
    }
    if (rok && !stations.empty()) {
        ESP_LOGI("AiPassport", "self-test: streaming radio %s for 5s...",
                 stations[0].url.c_str());
        Application::GetInstance().CloseAudioChannel();
        MusicPlayer player(board->GetAudioCodec());
        std::atomic<int> events{0};
        player.SetCallback([&events](MusicPlayer::Event ev, const MediaTrack& t, int a) {
            events++;
            ESP_LOGI("AiPassport", "  radio event %d arg=%d (%s)", (int)ev, a, t.title.c_str());
        });
        player.Play(stations[0]);
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI("AiPassport", "self-test: radio after 5s playing=%d events=%d heap=%u",
                 (int)player.IsPlaying(), (int)events.load(),
                 (unsigned)esp_get_free_heap_size());
        player.Stop();
    }
    ESP_LOGI("AiPassport", "self-test: done, free heap %u",
             (unsigned)esp_get_free_heap_size());
        vTaskDelete(nullptr);
}

// Serial console: "mt search <query>", "mt list <archive-id>", "mt play <url> [secs]"
static int MediaTestCmd(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: mt search <q> | mt list <id> | mt play <url> [secs]\n");
        return 1;
    }
    std::string sub = argv[1];
    std::string a = argc > 2 ? argv[2] : "";
    std::string b = argc > 3 ? argv[3] : "";
    if (g_board_for_console != nullptr) {
        g_board_for_console->DebugMedia(sub, a, b);
    }
    return 0;
}

static void StartMediaConsole() {
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "passport> ";
    repl_config.max_cmdline_length = 256;
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t* repl = nullptr;
    if (esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl) != ESP_OK) {
        return;
    }
    const esp_console_cmd_t cmd = {
        .command = "mt",
        .help = "media test: mt search <q> | mt list <id> | mt play <url> [secs]",
        .hint = nullptr,
        .func = MediaTestCmd,
        .argtable = nullptr,
    };
    esp_console_cmd_register(&cmd);
    esp_console_start_repl(repl);
    ESP_LOGI(TAG, "serial console ready (try: mt search asgore)");
}

DECLARE_BOARD(AiPassportBoard);

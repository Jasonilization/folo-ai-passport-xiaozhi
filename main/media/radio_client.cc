#include "radio_client.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <cctype>

#define TAG "RadioClient"

namespace {

std::string UrlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Fixed-buffer GET (exceptions are disabled in this build, so no throwing
// std::string growth in the network path).
struct RadioResponse {
    char* data = nullptr;
    size_t len = 0;
    size_t cap = 0;
};

esp_err_t RadioHttpEventHandler(esp_http_client_event_t* evt) {
    auto* resp = static_cast<RadioResponse*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp != nullptr && resp->data != nullptr) {
        size_t room = resp->cap - 1 - resp->len;
        size_t n = evt->data_len < room ? evt->data_len : room;
        memcpy(resp->data + resp->len, evt->data, n);
        resp->len += n;
    }
    return ESP_OK;
}

bool HttpGet(const std::string& url, RadioResponse& resp, size_t cap = 24 * 1024) {
    resp.data = static_cast<char*>(malloc(cap));
    if (resp.data == nullptr) {
        return false;
    }
    resp.cap = cap;
    resp.len = 0;
    resp.data[0] = '\0';
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 10000;
    config.event_handler = RadioHttpEventHandler;
    config.user_data = &resp;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    auto client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    resp.data[resp.len] = '\0';
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET failed (err=%d status=%d)", err, status);
        return false;
    }
    return true;
}

}  // namespace

bool RadioClient::Search(const std::string& query, int limit, std::vector<MediaTrack>& out) {
    try {
        return SearchImpl(query, limit, out);
    } catch (...) {
        ESP_LOGW(TAG, "search threw (out of memory?)");
        out.clear();
        return false;
    }
}

bool RadioClient::SearchImpl(const std::string& query, int limit, std::vector<MediaTrack>& out) {
    out.clear();
    // GET works for this endpoint too (some mirrors reject POST). Try several
    // mirrors: individual ones occasionally refuse connections.
    static const char* kHosts[] = {
        "de1.api.radio-browser.info",
        "de2.api.radio-browser.info",
        "all.api.radio-browser.info",
        "fi1.api.radio-browser.info",
    };
    for (const char* host : kHosts) {
        // Plain HTTP: radio-browser serves both, and the C3 has little heap
        // for a second TLS session.
        std::string url = std::string("http://") + host +
                          "/json/stations/search?name=" + UrlEncode(query) + "&limit=" +
                          std::to_string(limit) + "&order=votes&reverse=true&hidebroken=true";
        RadioResponse resp;
        if (!HttpGet(url, resp)) {
            free(resp.data);
            continue;  // try next mirror
        }
        cJSON* root = cJSON_ParseWithLength(resp.data, resp.len);
        free(resp.data);
        if (root == nullptr) {
            continue;
        }
    if (cJSON_IsArray(root)) {
        cJSON* station = nullptr;
        cJSON_ArrayForEach(station, root) {
            cJSON* name = cJSON_GetObjectItem(station, "name");
            cJSON* resolved = cJSON_GetObjectItem(station, "url_resolved");
            if (!cJSON_IsString(name) || !cJSON_IsString(resolved)) {
                continue;
            }
            MediaTrack track;
            track.title = name->valuestring;
            cJSON* country = cJSON_GetObjectItem(station, "country");
            track.artist = cJSON_IsString(country) ? country->valuestring : "";
            track.url = resolved->valuestring;
            track.is_stream = true;
            out.push_back(std::move(track));
        }
    }
        cJSON_Delete(root);
        return true;  // empty result is valid, not an error
    }
    return false;  // all mirrors failed
}
#include "archive_client.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include <cctype>
#include <cstring>

#define TAG "ArchiveClient"

namespace {

std::string UrlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Fixed-buffer GET: this firmware is built with C++ exceptions disabled, so a
// throwing std::string/std::vector allocation (heap pressure during TLS) would
// abort the whole device. All network paths here use malloc'd buffers only.
struct HttpResponse {
    char* data = nullptr;   // malloc'd, NUL-terminated
    size_t len = 0;
    size_t cap = 0;
    bool truncated = false;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    auto* resp = static_cast<HttpResponse*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp != nullptr && resp->data != nullptr) {
        size_t room = resp->cap - 1 - resp->len;
        size_t n = evt->data_len < room ? evt->data_len : room;
        memcpy(resp->data + resp->len, evt->data, n);
        resp->len += n;
        if (n < (size_t)evt->data_len) {
            resp->truncated = true;
        }
    }
    return ESP_OK;
}

// TLS certificate validation needs a sane clock; SNTP syncs shortly after
// WiFi connects, so wait for it before the first HTTPS request.
static void WaitForTimeSync() {
    for (int i = 0; i < 20 && time(nullptr) < 1735689600; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool HttpGet(const std::string& url, HttpResponse& resp, size_t cap = 24 * 1024,
             const char* range = nullptr) {
    WaitForTimeSync();
    // The C3 heap is small and fragmented; if the full buffer won't fit, try
    // smaller windows rather than failing outright.
    while (cap >= 8 * 1024) {
        resp.data = static_cast<char*>(malloc(cap));
        if (resp.data != nullptr) {
            break;
        }
        cap /= 2;
    }
    if (resp.data == nullptr) {
        ESP_LOGW(TAG, "GET %s: no heap for %u bytes", url.c_str(), (unsigned)cap);
        return false;
    }
    resp.cap = cap;
    resp.len = 0;
    resp.truncated = false;
    resp.data[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 10000;
    config.event_handler = HttpEventHandler;
    config.user_data = &resp;
    config.buffer_size = 2048;
    config.disable_auto_redirect = false;
    // Verify HTTPS against the built-in CA bundle (without this the TLS
    // connect fails with ESP_ERR_HTTP_CONNECT).
    config.crt_bundle_attach = esp_crt_bundle_attach;
    auto client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    if (range != nullptr) {
        esp_http_client_set_header(client, "Range", range);
    }
    esp_err_t err = ESP_FAIL;
    int status = 0;
    // DNS on this link is flaky (getaddrinfo sometimes returns 202), so retry
    // the whole request a few times with a pause between attempts.
    for (int attempt = 0; attempt < 4; attempt++) {
        client = esp_http_client_init(&config);
        if (client == nullptr) break;
        if (range != nullptr) {
            esp_http_client_set_header(client, "Range", range);
        }
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        client = nullptr;
        if (err == ESP_OK && (status == 200 || status == 206)) {
            break;
        }
        if (attempt < 3) {
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    resp.data[resp.len] = '\0';
    if (err != ESP_OK || (status != 200 && status != 206)) {
        ESP_LOGW(TAG, "GET %s failed (err=%d status=%d len=%u)", url.c_str(), err, status,
                 (unsigned)resp.len);
        return false;
    }
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

bool ArchiveClient::Search(const std::string& query, int limit,
                           std::vector<MediaItem>& out) {
    try {
        return SearchImpl(query, limit, out);
    } catch (...) {
        ESP_LOGW(TAG, "search threw (out of memory?)");
        out.clear();
        return false;
    }
}

bool ArchiveClient::SearchImpl(const std::string& query, int limit,
                               std::vector<MediaItem>& out) {
    out.clear();
    // mediatype:audio keeps noise down; q matches title/description fields.
    std::string url = "https://archive.org/advancedsearch.php?q=(" + UrlEncode(query) +
                      ")+AND+mediatype%3Aaudio&fl%5B%5D=identifier&fl%5B%5D=title&fl%5B%5D="
                      "creator&rows=" +
                      std::to_string(limit) +
                      "&sort%5B%5D=downloads+desc&output=json";
    HttpResponse resp;
    if (!HttpGet(url, resp, 24 * 1024)) {
        free(resp.data);
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(resp.data, resp.len);
    free(resp.data);
    if (root == nullptr) {
        return false;
    }
    cJSON* docs = cJSON_GetObjectItem(root, "response");
    if (docs != nullptr) {
        docs = cJSON_GetObjectItem(docs, "docs");
    }
    if (cJSON_IsArray(docs)) {
        cJSON* doc = nullptr;
        cJSON_ArrayForEach(doc, docs) {
            MediaItem item;
            cJSON* id = cJSON_GetObjectItem(doc, "identifier");
            cJSON* title = cJSON_GetObjectItem(doc, "title");
            if (cJSON_IsString(id) && cJSON_IsString(title)) {
                item.identifier = id->valuestring;
                item.title = title->valuestring;
                out.push_back(std::move(item));
            }
        }
    }
    cJSON_Delete(root);
    return true;  // empty result is valid, not an error
}

bool ArchiveClient::ListFiles(const std::string& identifier, std::vector<MediaFile>& out) {
    try {
        return ListFilesImpl(identifier, out);
    } catch (...) {
        ESP_LOGW(TAG, "listfiles threw (out of memory?)");
        out.clear();
        return false;
    }
}

bool ArchiveClient::ListFilesImpl(const std::string& identifier, std::vector<MediaFile>& out) {
    out.clear();
    // Stream-scan the metadata JSON in chunks: music items can have metadata
    // well over 200KB, which will never fit the C3 heap in one buffer. We only
    // keep the audio file entries we find (bounded), never the whole document.
    const std::string url = "https://archive.org/metadata/" + identifier;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 10000;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = nullptr;
    bool opened = false;
    for (int attempt = 0; attempt < 4 && !opened; attempt++) {
        client = esp_http_client_init(&config);
        if (client == nullptr) break;
        if (esp_http_client_open(client, 0) == ESP_OK) {
            opened = true;
        } else {
            esp_http_client_cleanup(client);
            client = nullptr;
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    if (!opened) {
        ESP_LOGW(TAG, "metadata open failed for %s", identifier.c_str());
        return false;
    }
    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        ESP_LOGW(TAG, "metadata status %d for %s",
                 esp_http_client_get_status_code(client), identifier.c_str());
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    constexpr size_t kChunk = 8 * 1024;
    constexpr size_t kCarry = 512;      // overlap so patterns split across chunks survive
    constexpr size_t kMaxFiles = 60;     // bound memory
    char* chunk = static_cast<char*>(malloc(kChunk));
    if (chunk == nullptr) {
        ESP_LOGW(TAG, "metadata chunk malloc failed, heap=%u", (unsigned)esp_get_free_heap_size());
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    ESP_LOGI(TAG, "metadata stream start, heap=%u", (unsigned)esp_get_free_heap_size());
    std::string window;
    window.reserve(kChunk + kCarry + 1);
    bool in_files = false;
    std::string pending_name;  // a name whose format/size may be in the next chunk
    auto read_json_string = [](const char*& q, std::string& out_str) -> bool {
        if (*q != '"') return false;
        q++;
        out_str.clear();
        while (*q && *q != '"') {
            if (*q == '\\' && q[1]) {
                char c = q[1];
                out_str.push_back(c == 'n' ? '\n' : c == 't' ? '\t' : c);
                q += 2;
            } else {
                out_str.push_back(*q++);
            }
        }
        if (*q == '"') { q++; return true; }
        return false;
    };
    auto classify = [](MediaFile& mf) -> bool {
        std::string l = mf.format;
        for (char& c : l) c = (char)tolower((unsigned char)c);
        std::string nl = mf.name;
        for (char& c : nl) c = (char)tolower((unsigned char)c);
        // Playable only: mp3 / ogg / flac (no WAV container decoder bundled).
        return l.find("mp3") != std::string::npos || l.find("ogg") != std::string::npos ||
               l.find("vorbis") != std::string::npos || l.find("flac") != std::string::npos ||
               nl.find(".mp3") != std::string::npos || nl.find(".ogg") != std::string::npos ||
               nl.find(".flac") != std::string::npos;
    };
    auto try_emit = [&](MediaFile& mf) {
        if (classify(mf)) {
            mf.url = "https://archive.org/download/" + identifier + "/" + UrlEncode(mf.name);
            out.push_back(std::move(mf));
        }
    };

    int r = 0;
    while ((r = esp_http_client_read(client, chunk, kChunk)) != 0 &&
           out.size() < kMaxFiles) {
        if (r < 0) {
            ESP_LOGW(TAG, "metadata read error %d", r);
            break;
        }
        window.assign(chunk, r);
        if (in_files) {
            // resolve pending name from previous chunk
            if (!pending_name.empty()) {
                MediaFile mf;
                mf.name = pending_name;
                const char* f = strstr(window.c_str(), "\"format\"");
                if (f) {
                    const char* fc = strchr(f, ':');
                    const char* fq = fc ? fc + 1 : f;
                    while (*fq == ' ') fq++;
                    read_json_string(fq, mf.format);
                }
                const char* sz = strstr(window.c_str(), "\"size\"");
                if (sz) mf.size = strtoll(sz + 6, nullptr, 10);
                try_emit(mf);
                pending_name.clear();
            }
        }
        const char* base = window.c_str();
        if (!in_files) {
            const char* f = strstr(base, "\"files\"");
            if (f) {
                in_files = true;
                base = strchr(f, '[');
                if (base == nullptr) break;
                base++;
            }
        }
        // scan for "name":"..." entries
        const char* p = base;
        while ((p = strstr(p, "\"name\"")) != nullptr && out.size() < kMaxFiles) {
            const char* colon = strchr(p, ':');
            if (colon == nullptr) break;
            const char* q = colon + 1;
            while (*q == ' ') q++;
            MediaFile mf;
            if (!read_json_string(q, mf.name)) break;
            // format/size usually follow within the same object
            const char* obj_end = strstr(q, "\"name\"");
            const char* f = strstr(q, "\"format\"");
            if (f && (!obj_end || f < obj_end)) {
                const char* fc = strchr(f, ':');
                const char* fq = fc ? fc + 1 : f;
                while (*fq == ' ') fq++;
                read_json_string(fq, mf.format);
            }
            const char* sz = strstr(q, "\"size\"");
            if (sz && (!obj_end || sz < obj_end)) {
                mf.size = strtoll(sz + 6, nullptr, 10);
            }
            if (mf.format.empty()) {
                // format may be in the next chunk; stash and resolve later
                pending_name = mf.name;
            } else {
                try_emit(mf);
            }
            p = q;
        }
        // carry the tail for cross-chunk patterns
        if (window.size() > kCarry) {
            window = window.substr(window.size() - kCarry);
        }
    }
    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!pending_name.empty()) {
        // unresolved tail entry; classify by extension alone
        MediaFile mf;
        mf.name = pending_name;
        try_emit(mf);
    }
    return true;  // empty list is valid, not an error
}

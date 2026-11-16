#include "archive_client.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <cctype>
#include <cstring>

#define TAG "ArchiveClient"

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

bool HttpGet(const std::string& url, HttpResponse& resp, size_t cap = 24 * 1024) {
    resp.data = static_cast<char*>(malloc(cap));
    if (resp.data == nullptr) {
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
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    resp.data[resp.len] = '\0';
    if (err != ESP_OK || status != 200) {
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
    std::string url = "https://archive.org/metadata/" + identifier;
    HttpResponse resp;
    // File listings for music items can be large; keep the cap modest because
    // the C3 has no PSRAM and TLS already eats heap. The filter below keeps
    // only audio entries.
    if (!HttpGet(url, resp, 64 * 1024)) {
        free(resp.data);
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(resp.data, resp.len);
    free(resp.data);
    if (root == nullptr) {
        return false;
    }
    cJSON* files = cJSON_GetObjectItem(root, "files");
    if (cJSON_IsArray(files)) {
        cJSON* file = nullptr;
        cJSON_ArrayForEach(file, files) {
            cJSON* name = cJSON_GetObjectItem(file, "name");
            cJSON* format = cJSON_GetObjectItem(file, "format");
            cJSON* size = cJSON_GetObjectItem(file, "size");
            if (!cJSON_IsString(name)) {
                continue;
            }
            std::string name_l = Lower(name->valuestring);
            std::string fmt = cJSON_IsString(format) ? Lower(format->valuestring) : "";
            // Playable audio only: mp3, ogg (vorbis), flac. WAV is excluded on
            // purpose: the bundled decoders have no RIFF/WAV container parser,
            // so a WAV pick would just fail at playback time.
            bool ok = fmt.find("mp3") != std::string::npos ||
                      fmt.find("ogg") != std::string::npos ||
                      fmt.find("vorbis") != std::string::npos ||
                      fmt.find("flac") != std::string::npos ||
                      name_l.find(".mp3") != std::string::npos ||
                      name_l.find(".ogg") != std::string::npos ||
                      name_l.find(".flac") != std::string::npos;
            if (!ok) {
                continue;
            }
            MediaFile mf;
            mf.name = name->valuestring;
            mf.url = "https://archive.org/download/" + identifier + "/" +
                     UrlEncode(mf.name);
            mf.format = fmt;
            if (cJSON_IsNumber(size)) {
                mf.size = static_cast<int64_t>(size->valuedouble);
            }
            out.push_back(std::move(mf));
        }
    }
    cJSON_Delete(root);
    return true;  // empty result is valid, not an error
}
#include "archive_client.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
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

bool HttpGet(const std::string& url, HttpResponse& resp, size_t cap = 24 * 1024,
             const char* range = nullptr) {
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
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
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
    std::string url = "https://archive.org/metadata/" + identifier;
    HttpResponse resp;
    // The "files" array sits at the END of the metadata JSON, which for music
    // collections is far bigger than the C3 heap allows in one shot. So fetch
    // only the tail via a Range request (206) and scan it tolerantly.
    size_t cap = 48 * 1024;
    if (esp_get_free_heap_size() > 160 * 1024) {
        cap = 96 * 1024;
    }
    char range[32];
    snprintf(range, sizeof(range), "bytes=-%u", (unsigned)cap);
    if (!HttpGet(url, resp, cap, range)) {
        free(resp.data);
        return false;
    }

    // Find the "files" array and scan its entries for name/format/size
    // without building a full cJSON tree (the tree is what OOMs on big items).
    const char* files = strstr(resp.data, "\"files\"");
    if (files == nullptr) {
        // Tail window missed the files array: retry with the head of the
        // document (covers small items whose metadata is short).
        free(resp.data);
        HttpResponse head;
        if (!HttpGet(url, head, 24 * 1024)) {
            free(head.data);
            return false;
        }
        resp.data = head.data;
        resp.len = head.len;
        resp.cap = head.cap;
        files = strstr(resp.data, "\"files\"");
        if (files == nullptr) {
            free(resp.data);
            return true;
        }
    }
    const char* p = strchr(files, '[');
    if (p == nullptr) {
        free(resp.data);
        return true;
    }
    auto read_json_string = [](const char*& q, std::string& out_str) -> bool {
        // q points at the opening quote of a JSON string value
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
    while ((p = strstr(p, "\"name\"")) != nullptr) {
        const char* colon = strchr(p, ':');
        if (colon == nullptr) break;
        const char* q = colon + 1;
        while (*q == ' ') q++;
        MediaFile mf;
        if (!read_json_string(q, mf.name)) break;
        // look ahead within this object for format/size (bounded window)
        const char* obj_end = strstr(q, "\"name\"");
        const char* window_end = obj_end ? obj_end : q + 512;
        const char* f = strstr(q, "\"format\"");
        if (f && f < window_end) {
            const char* fc = strchr(f, ':');
            const char* fq = fc ? fc + 1 : f;
            while (*fq == ' ') fq++;
            read_json_string(fq, mf.format);
            for (char& c : mf.format) c = (char)tolower((unsigned char)c);
        }
        const char* sz = strstr(q, "\"size\"");
        if (sz && sz < window_end) {
            mf.size = strtoll(sz + 6, nullptr, 10);
        }
        std::string name_l = mf.name;
        for (char& c : name_l) c = (char)tolower((unsigned char)c);
        // Playable audio only: mp3, ogg (vorbis), flac. WAV is excluded on
        // purpose: the bundled decoders have no RIFF/WAV container parser.
        bool ok = mf.format.find("mp3") != std::string::npos ||
                  mf.format.find("ogg") != std::string::npos ||
                  mf.format.find("vorbis") != std::string::npos ||
                  mf.format.find("flac") != std::string::npos ||
                  name_l.find(".mp3") != std::string::npos ||
                  name_l.find(".ogg") != std::string::npos ||
                  name_l.find(".flac") != std::string::npos;
        if (ok) {
            mf.url = "https://archive.org/download/" + identifier + "/" + UrlEncode(mf.name);
            out.push_back(std::move(mf));
        }
        p = q;
    }
    free(resp.data);
    return true;  // empty list is valid, not an error
}

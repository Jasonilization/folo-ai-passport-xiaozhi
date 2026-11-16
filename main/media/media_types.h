#ifndef MEDIA_TYPES_H
#define MEDIA_TYPES_H

#include <string>

// One playable track or radio stream.
struct MediaTrack {
    std::string title;
    std::string artist;
    std::string url;
    int duration_sec = 0;
    bool is_stream = false;  // radio: unknown/endless duration
};

// One result of an archive.org item search.
struct MediaItem {
    std::string identifier;
    std::string title;
};

// One playable file inside an archive.org item ("folder").
struct MediaFile {
    std::string name;
    std::string url;
    std::string format;  // "mp3", "ogg", "flac", "wav"...
    int64_t size = 0;
};

#endif
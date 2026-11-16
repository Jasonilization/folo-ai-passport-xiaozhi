#ifndef ARCHIVE_CLIENT_H
#define ARCHIVE_CLIENT_H

#include "media_types.h"

#include <string>
#include <vector>

// Thin client for the Internet Archive (archive.org) APIs.
//
// All calls are plain HTTP + JSON; no API key needed. Search returns items
// ("folders"), ListFiles returns the audio files inside one item so the UI can
// browse into it like a directory.
class ArchiveClient {
public:
    // Search archive.org for audio items matching the query. Returns false
    // only on network/parse failure; an empty result returns true.
    bool Search(const std::string& query, int limit, std::vector<MediaItem>& out);

    // List the playable audio files inside one archive.org item. Returns
    // false only on network/parse failure; an item with no audio files
    // returns true with an empty list.
    bool ListFiles(const std::string& identifier, std::vector<MediaFile>& out);

private:
    bool SearchImpl(const std::string& query, int limit, std::vector<MediaItem>& out);
    bool ListFilesImpl(const std::string& identifier, std::vector<MediaFile>& out);
};

#endif
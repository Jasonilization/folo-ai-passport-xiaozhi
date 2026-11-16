#ifndef RADIO_CLIENT_H
#define RADIO_CLIENT_H

#include "media/media_types.h"

#include <string>
#include <vector>

// Thin client for the free radio-browser.info directory (no API key).
// Stations are listed by name/tag search, ordered by votes.
class RadioClient {
public:
    // Search stations by free text (name, tag, country...). Returns false
    // only if every mirror failed; an empty result returns true.
    bool Search(const std::string& query, int limit, std::vector<MediaTrack>& out);

private:
    bool SearchImpl(const std::string& query, int limit, std::vector<MediaTrack>& out);
};

#endif
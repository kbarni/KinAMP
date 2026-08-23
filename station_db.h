#ifndef STATION_DB_H
#define STATION_DB_H

// Station list handling behind the GTK player's station manager: searching
// the bundled radio-browser dump, and unwrapping .pls/.m3u links down to a
// stream URL. Everything here is UI-free - the callers do their own
// prompting through the StreamChooser callback.

#include <string>
#include <vector>

struct Station {
    std::string name;
    std::string url;
};

namespace stationdb {

// ---------------------------------------------------------- station database

// The bundled allStations.json, looked up next to the binary as well as in the
// working directory. Empty when none of the candidates exists.
std::string database_path();

// Case-insensitive substring match on the station name. `limit` caps the
// result count (0 = no cap); `truncated` tells whether the cap was hit.
// False means the database could not be read.
bool search(const std::string &term, std::vector<Station> &results,
            size_t limit = 0, bool *truncated = NULL);

// ------------------------------------------------------------- URL handling

// Formats the player cannot decode yet. AAC/ADTS is handled by FAAD2, but HLS
// (.m3u8) is a segmented transport rather than a plain stream.
bool is_unsupported_stream(const std::string &url);

// Whether resolve_playlist_url() will hit the network for this URL. Callers
// use it to put up a "Resolving..." notice only when something is downloaded.
bool needs_resolving(const std::string &url);

// Fetch at most max_bytes of a URL. fork/exec rather than popen, so station
// URLs (which come out of a downloaded database) never reach a shell.
std::string http_fetch(const std::string &url, size_t max_bytes, int timeout_sec);

// Called when a playlist holds more than one entry. Returning false means the
// user cancelled; otherwise `index` selects one of `streams`.
typedef bool (*StreamChooser)(const std::vector<std::string> &streams,
                              size_t &index, void *user_data);

enum ResolveResult {
    RESOLVE_OK,        // station.url is a stream URL
    RESOLVE_CANCELLED, // the chooser said no
    RESOLVE_ERROR      // nothing usable; `error` says what happened
};

// Follows .pls/.m3u (and content-sniffed) playlists down to a real stream URL.
ResolveResult resolve_playlist_url(Station &station, StreamChooser chooser,
                                   void *user_data, std::string *error);

// Pieces of the above, exposed for callers that want them on their own.
bool case_insensitive_contains(const std::string &str, const std::string &sub);
bool url_has_ext(const std::string &url, const std::string &ext);
bool looks_like_playlist(const std::string &body);
std::vector<std::string> parse_playlist(const std::string &body, const std::string &base_url);

} // namespace stationdb

#endif // STATION_DB_H

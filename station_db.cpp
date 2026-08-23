#include "station_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <algorithm>

namespace stationdb {

namespace {

const char *DB_FILE_NAME = "allStations.json";
const int MAX_RESOLVE_DEPTH = 3;

bool ends_with_ci(const std::string &str, const std::string &suffix) {
    if (str.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
        [](unsigned char a, unsigned char b){ return std::tolower(a) == std::tolower(b); });
}

// Many station URLs hide the extension behind a query string
// (e.g. http://host/listen.pls?sid=25), so test the path only.
std::string url_path(const std::string &url) {
    size_t cut = url.find_first_of("?#");
    return (cut == std::string::npos) ? url : url.substr(0, cut);
}

std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

// Playlist entries may be relative to the playlist's own location.
std::string resolve_relative(const std::string &base, const std::string &ref) {
    if (ref.find("://") != std::string::npos) return ref;
    std::string b = url_path(base);
    size_t scheme = b.find("://");
    if (scheme == std::string::npos) return ref;

    if (!ref.empty() && ref[0] == '/') {
        size_t slash = b.find('/', scheme + 3);
        return (slash == std::string::npos ? b : b.substr(0, slash)) + ref;
    }
    size_t slash = b.find_last_of('/');
    if (slash == std::string::npos || slash < scheme + 3) return b + "/" + ref;
    return b.substr(0, slash + 1) + ref;
}

// Directory the running binary lives in, so the database is found no matter
// what the working directory is. Empty when /proc is not readable.
std::string executable_dir() {
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "";
    buf[len] = 0;
    std::string path(buf);
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    return path.substr(0, slash + 1);
}

// One unwrapping step: does `url` point at a playlist, and if so, at what.
enum StepResult {
    STEP_STREAM,        // use the URL as it is
    STEP_STREAMS_FOUND, // `streams` holds what the playlist listed
    STEP_FETCH_FAILED,  // named playlist that could not be downloaded
    STEP_EMPTY          // playlist without a usable entry
};

StepResult resolve_step(const std::string &url, std::vector<std::string> &streams) {
    bool named_playlist = url_has_ext(url, ".pls") || url_has_ext(url, ".m3u");
    if (!needs_resolving(url)) return STEP_STREAM;

    std::string body = http_fetch(url, named_playlist ? 65536 : 2048, 3);
    if (body.empty()) {
        // A probe that came back empty tells us nothing: treat it as a stream.
        return named_playlist ? STEP_FETCH_FAILED : STEP_STREAM;
    }
    if (!looks_like_playlist(body)) return STEP_STREAM;

    streams = parse_playlist(body, url);
    if (streams.empty()) return STEP_EMPTY;
    return STEP_STREAMS_FOUND;
}

} // namespace

bool case_insensitive_contains(const std::string &str, const std::string &sub) {
    if (sub.empty()) return true;
    auto it = std::search(
        str.begin(), str.end(),
        sub.begin(), sub.end(),
        [](unsigned char ch1, unsigned char ch2) {
            return std::tolower(ch1) == std::tolower(ch2);
        }
    );
    return it != str.end();
}

bool url_has_ext(const std::string &url, const std::string &ext) {
    return ends_with_ci(url_path(url), ext);
}

std::string database_path() {
    std::vector<std::string> candidates;
    const char *env = getenv("KINAMP_STATIONS_DB");
    if (env && *env) candidates.push_back(env);

    std::string bin = executable_dir();
    if (!bin.empty()) {
        candidates.push_back(bin + DB_FILE_NAME);
        candidates.push_back(bin + "assets/" + DB_FILE_NAME);
    }
    candidates.push_back(DB_FILE_NAME);
    candidates.push_back(std::string("assets/") + DB_FILE_NAME);

    for (size_t i = 0; i < candidates.size(); ++i) {
        FILE *f = fopen(candidates[i].c_str(), "r");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return "";
}

// Simple manual JSON parser for an array of arrays of strings:
// [["Name","URL"],...]. Naive on purpose - the file is a fixed dump.
bool search(const std::string &term, std::vector<Station> &results,
            size_t limit, bool *truncated) {
    if (truncated) *truncated = false;

    std::string path = database_path();
    if (path.empty()) return false;

    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;

    // Read the whole file into memory (it's around 3-4MB).
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(f);
        return false;
    }

    char *buffer = (char*)malloc(fsize + 1);
    if (!buffer) {
        fclose(f);
        return false;
    }
    size_t got = fread(buffer, 1, fsize, f);
    buffer[got] = 0;
    fclose(f);

    char *cursor = buffer;
    while (*cursor) {
        // Find start of an entry [
        char *entry_start = strchr(cursor, '[');
        if (!entry_start) break;
        cursor = entry_start + 1;

        // Name: first quoted string of the entry. Escaped quotes are not
        // handled - the dump has none.
        char *name_start_quote = strchr(cursor, '"');
        if (!name_start_quote) break;
        char *name_end_quote = strchr(name_start_quote + 1, '"');
        if (!name_end_quote) break;
        std::string name(name_start_quote + 1, name_end_quote - (name_start_quote + 1));
        cursor = name_end_quote + 1;

        // URL: the second string.
        char *url_start_quote = strchr(cursor, '"');
        if (!url_start_quote) break;
        char *url_end_quote = strchr(url_start_quote + 1, '"');
        if (!url_end_quote) break;
        std::string url(url_start_quote + 1, url_end_quote - (url_start_quote + 1));
        cursor = url_end_quote + 1;

        if (case_insensitive_contains(name, term)) {
            Station s;
            s.name = name;
            s.url = url;
            results.push_back(s);
            if (limit && results.size() >= limit) {
                if (truncated) *truncated = true;
                break;
            }
        }
    }

    free(buffer);
    return true;
}

std::string http_fetch(const std::string &url, size_t max_bytes, int timeout_sec) {
    int pipefd[2];
    if (pipe(pipefd) == -1) return "";

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }

    if (pid == 0) { // Child
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%d", timeout_sec);
        execlp("wget", "wget", "-q", "-T", tbuf, "-t", "1",
               "--no-check-certificate", "-O", "-", url.c_str(), (char*)NULL);
        _exit(1);
    }

    close(pipefd[1]);
    std::string body;
    char buf[1024];
    while (body.size() < max_bytes) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        body.append(buf, n);
    }
    close(pipefd[0]);

    // We may have stopped short of the end (or of an endless audio stream).
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return body;
}

// Decide from the body, not the URL: a .m3u can serve [playlist] and plenty of
// extensionless endpoints (listen.php?port=...) are playlists too.
bool looks_like_playlist(const std::string &body) {
    std::string head = trim(body.substr(0, 512));
    if (head.empty()) return false;
    if (case_insensitive_contains(head.substr(0, 32), "[playlist]")) return true;
    if (case_insensitive_contains(head.substr(0, 32), "#EXTM3U")) return true;
    // A bare list of stream URLs is a valid (extension-less) M3U.
    return head.compare(0, 7, "http://") == 0 || head.compare(0, 8, "https://") == 0;
}

std::vector<std::string> parse_playlist(const std::string &body, const std::string &base_url) {
    std::vector<std::string> urls;
    bool is_pls = case_insensitive_contains(trim(body).substr(0, 32), "[playlist]");

    size_t pos = 0;
    bool first_line = true;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string line = trim(body.substr(pos, eol - pos));
        pos = eol + 1;

        if (first_line) {
            first_line = false;
            if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line = trim(line.substr(3));
        }
        if (line.empty()) continue;

        std::string entry;
        if (is_pls) {
            // FileN=url ; skip TitleN= / LengthN= / NumberOfEntries=
            if (line.size() > 4 &&
                tolower((unsigned char)line[0]) == 'f' &&
                tolower((unsigned char)line[1]) == 'i' &&
                tolower((unsigned char)line[2]) == 'l' &&
                tolower((unsigned char)line[3]) == 'e') {
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                entry = trim(line.substr(eq + 1));
            }
        } else {
            if (line[0] == '#') continue; // #EXTM3U, #EXTINF, comments
            entry = line;
        }

        if (entry.empty()) continue;
        entry = resolve_relative(base_url, entry);
        if (std::find(urls.begin(), urls.end(), entry) == urls.end()) {
            urls.push_back(entry);
        }
    }
    return urls;
}

bool is_unsupported_stream(const std::string &url) {
    return url_has_ext(url, ".m3u8");
}

bool needs_resolving(const std::string &url) {
    bool named_playlist = url_has_ext(url, ".pls") || url_has_ext(url, ".m3u");
    bool known_audio = url_has_ext(url, ".mp3") || url_has_ext(url, ".aac") ||
                       url_has_ext(url, ".ogg") || url_has_ext(url, ".opus") ||
                       url_has_ext(url, ".flac") || url_has_ext(url, ".m3u8");

    // Only probe when the name tells us nothing, so we don't pull audio from
    // every station just to classify it.
    return named_playlist || !known_audio;
}

ResolveResult resolve_playlist_url(Station &station, StreamChooser chooser,
                                   void *user_data, std::string *error) {
    // A playlist may point at another playlist (shoutcast tunein-station.pls),
    // but stop unwrapping eventually and use whatever we ended up with.
    for (int depth = 0; depth < MAX_RESOLVE_DEPTH; ++depth) {
        std::vector<std::string> streams;
        switch (resolve_step(station.url, streams)) {
            case STEP_STREAM:
                return RESOLVE_OK;
            case STEP_FETCH_FAILED:
                if (error) *error = "Could not download the playlist.";
                return RESOLVE_ERROR;
            case STEP_EMPTY:
                if (error) *error = "No streams found in playlist.";
                return RESOLVE_ERROR;
            case STEP_STREAMS_FOUND:
                break;
        }

        size_t index = 0;
        if (streams.size() > 1) {
            if (!chooser) return RESOLVE_CANCELLED;
            if (!chooser(streams, index, user_data)) return RESOLVE_CANCELLED;
            if (index >= streams.size()) return RESOLVE_CANCELLED;
        }
        station.url = streams[index];
    }
    return RESOLVE_OK;
}

} // namespace stationdb

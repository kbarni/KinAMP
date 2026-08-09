#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>
#include <string>
#include <algorithm>

// Function prototypes
std::string get_config_path(const char* filename);
void clear_screen();
void wait_for_enter();
void show_main_menu();
void list_stations();
void add_station();
void add_station_manual();
void remove_station();
std::string to_lower(const std::string& str);
bool case_insensitive_contains(const std::string& str, const std::string& sub);

bool ends_with_ci(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
        [](unsigned char a, unsigned char b){ return std::tolower(a) == std::tolower(b); });
}

// Many station URLs hide the extension behind a query string
// (e.g. http://host/listen.pls?sid=25), so test the path only.
std::string url_path(const std::string& url) {
    size_t cut = url.find_first_of("?#");
    return (cut == std::string::npos) ? url : url.substr(0, cut);
}

bool url_has_ext(const std::string& url, const std::string& ext) {
    return ends_with_ci(url_path(url), ext);
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

// Playlist entries may be relative to the playlist's own location.
std::string resolve_relative(const std::string& base, const std::string& ref) {
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

struct Station {
    std::string name;
    std::string url;
};

std::vector<Station> user_stations;
const char* STATIONS_DB_FILE = "assets/allStations.json"; // Relative to executable usually, check logic later
const char* CONFIG_FILE = ".kinamp_radio.txt";

std::string get_config_path(const char* filename) {
    return std::string(filename);
}

void load_user_stations() {
    user_stations.clear();
    std::string path = get_config_path(CONFIG_FILE);
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0; // trim newline
        char* sep = strchr(line, '|');
        if (sep) {
            *sep = 0;
            Station s;
            s.name = line;
            s.url = sep + 1;
            user_stations.push_back(s);
        }
    }
    fclose(f);
}

void save_user_stations() {
    std::string path = get_config_path(CONFIG_FILE);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        printf("Error saving stations to %s\n", path.c_str());
        return;
    }

    for (const auto& s : user_stations) {
        fprintf(f, "%s|%s\n", s.name.c_str(), s.url.c_str());
    }
    fclose(f);
    printf("Stations saved.\n");
}

// Simple manual JSON parser for array of arrays of strings: [["Name","URL"],...]
// Returns true if parsing successful (even if empty)
bool search_json_db(const std::string& term, std::vector<Station>& results) {
    FILE* f = fopen(STATIONS_DB_FILE, "r");
    if (!f) {
        // Try looking in current dir if assets/ failed
        f = fopen("allStations.json", "r");
        if (!f) {
            printf("Error: Could not open stations database (allStations.json or assets/allStations.json).\n");
            return false;
        }
    }

    // Read entire file into memory (it's around 3-4MB)
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(fsize + 1);
    if (!buffer) {
        printf("Error: Not enough memory to load station database.\n");
        fclose(f);
        return false;
    }
    fread(buffer, 1, fsize, f);
    buffer[fsize] = 0;
    fclose(f);

    char* cursor = buffer;
    
    // Very naive parser tailored for this specific file format
    while (*cursor) {
        // Find start of an entry [
        char* entry_start = strchr(cursor, '[');
        if (!entry_start) break;
        cursor = entry_start + 1;

        // Find first quote for Name
        char* name_start_quote = strchr(cursor, '"');
        if (!name_start_quote) break;
        
        // Find closing quote for Name
        // Handle escaped quotes? The file seems simple, but let's be slightly careful.
        // Assuming no escaped quotes for simplicity as per wiki excerpt, 
        // but robust json parsing is hard. We'll just look for next ". 
        char* name_end_quote = strchr(name_start_quote + 1, '"');
        if (!name_end_quote) break;

        // Extract Name
        std::string name(name_start_quote + 1, name_end_quote - (name_start_quote + 1));
        
        cursor = name_end_quote + 1;

        // Find second string (URL)
        char* url_start_quote = strchr(cursor, '"');
        if (!url_start_quote) break;
        
        char* url_end_quote = strchr(url_start_quote + 1, '"');
        if (!url_end_quote) break;

        std::string url(url_start_quote + 1, url_end_quote - (url_start_quote + 1));

        cursor = url_end_quote + 1;

        // Check match
        if (case_insensitive_contains(name, term)) {
            Station s;
            s.name = name;
            s.url = url;
            results.push_back(s);
        }
    }

    free(buffer);
    return true;
}

std::string to_lower(const std::string& str) {
    std::string lower_str = str;
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return lower_str;
}

bool case_insensitive_contains(const std::string& str, const std::string& sub) {
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

void clear_screen() {
    // ANSI escape code to clear screen
    printf("\033[H\033[J");
}

void wait_for_enter() {
    printf("\nPress Enter to continue...");
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
    // if buffer was empty, getchar waits. If buffer had newline, it returns.
    // We might need to drain buffer if previous scanf left a newline.
}

// Flush stdin helper
void flush_input() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

int main() {
    load_user_stations();
    while (1) {
        show_main_menu();
    }
    return 0;
}

void show_main_menu() {
    clear_screen();
    printf("Main menu\n");
    printf("=========\n\n");
    printf("1 - List stations\n");
    printf("2 - Add station\n");
    printf("3 - Remove station\n");
    printf("4 - Add station manually\n");
    printf("Q - Quit\n\n");
    printf("Your choice: ");

    char choice[10];
    if (!fgets(choice, sizeof(choice), stdin)) exit(0); // EOF: don't spin forever

    switch (choice[0]) {
        case '1': list_stations(); break;
        case '2': add_station(); break;
        case '3': remove_station(); break;
        case '4': add_station_manual(); break;
        case 'q':
        case 'Q': exit(0);
        default: break;
    }
}

void list_stations() {
    clear_screen();
    printf("Radio Stations\n");
    printf("==============\n\n");
    if (user_stations.empty()) {
        printf("(No stations added yet)\n");
    } else {
        for (size_t i = 0; i < user_stations.size(); ++i) {
            printf("%zu. %s\n   --> %s\n", i + 1, user_stations[i].name.c_str(), user_stations[i].url.c_str());
        }
    }
    wait_for_enter();
}

// Fetch at most max_bytes of a URL. Uses fork/exec rather than popen so that
// station URLs (which come from a downloaded database) are never seen by a shell.
std::string http_fetch(const std::string& url, size_t max_bytes, int timeout_sec) {
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
bool looks_like_playlist(const std::string& body) {
    std::string head = trim(body.substr(0, 512));
    if (head.empty()) return false;
    if (case_insensitive_contains(head.substr(0, 32), "[playlist]")) return true;
    if (case_insensitive_contains(head.substr(0, 32), "#EXTM3U")) return true;
    // A bare list of stream URLs is a valid (extension-less) M3U.
    return head.compare(0, 7, "http://") == 0 || head.compare(0, 8, "https://") == 0;
}

std::vector<std::string> parse_playlist(const std::string& body, const std::string& base_url) {
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

// Formats the player cannot decode yet.
bool is_unsupported_stream(const std::string& url) {
    return url_has_ext(url, ".aac") || url_has_ext(url, ".m3u8");
}

// Follows .pls/.m3u (and content-sniffed) playlists down to a real stream URL.
// Returns false if the user cancelled or nothing usable was found.
bool resolve_playlist_url(Station& station, int depth = 0) {
    const int MAX_DEPTH = 3;
    if (depth >= MAX_DEPTH) return true; // stop unwrapping, use what we have

    bool named_playlist = url_has_ext(station.url, ".pls") || url_has_ext(station.url, ".m3u");
    bool known_audio = url_has_ext(station.url, ".mp3") || url_has_ext(station.url, ".aac") ||
                       url_has_ext(station.url, ".ogg") || url_has_ext(station.url, ".opus") ||
                       url_has_ext(station.url, ".flac") || url_has_ext(station.url, ".m3u8");

    // Only probe when the name gives us nothing to go on, so we don't pull audio
    // from every station just to classify it.
    if (!named_playlist && known_audio) return true;

    printf("Resolving playlist...\n");
    fflush(stdout);
    std::string body = http_fetch(station.url, named_playlist ? 65536 : 2048, 3);
    if (body.empty()) {
        if (named_playlist) {
            printf("Could not download the playlist.\n");
            wait_for_enter();
            return false;
        }
        return true; // probe failed; treat as a direct stream
    }
    if (!looks_like_playlist(body)) return true;

    std::vector<std::string> streams = parse_playlist(body, station.url);
    if (streams.empty()) {
        printf("No streams found in playlist.\n");
        wait_for_enter();
        return false;
    }

    if (streams.size() == 1) {
        printf("  -> %s\n", streams[0].c_str());
        station.url = streams[0];
    } else {
        clear_screen();
        printf("Select stream from playlist:\n");
        for (size_t k = 0; k < streams.size(); ++k) {
            printf("%zu. %s%s\n", k + 1, streams[k].c_str(),
                   is_unsupported_stream(streams[k]) ? "  (unsupported)" : "");
        }
        printf("c. Cancel\n");
        printf("Choice: ");

        char subinput[10];
        if (!fgets(subinput, sizeof(subinput), stdin)) return false;
        if (!isdigit((unsigned char)subinput[0])) return false;
        size_t subchoice = atoi(subinput);
        if (subchoice < 1 || subchoice > streams.size()) return false;
        station.url = streams[subchoice - 1];
    }

    // A playlist may point at another playlist (shoutcast tunein-station.pls).
    return resolve_playlist_url(station, depth + 1);
}

void add_station() {
    clear_screen();
    printf("Add station\n");
    printf("===========\n\n");
    printf("Please enter the search term: ");
    
    char term_buffer[256];
    if (!fgets(term_buffer, sizeof(term_buffer), stdin)) return;
    term_buffer[strcspn(term_buffer, "\r\n")] = 0;
    
    if (strlen(term_buffer) == 0) return;

    std::string term = term_buffer;
    std::vector<Station> found;
    printf("Searching...\n");
    if (!search_json_db(term, found)) return;

    if (found.empty()) {
        printf("No stations found matching '%s'.\n", term.c_str());
        wait_for_enter();
        return;
    }

    size_t page = 0;
    const size_t PAGE_SIZE = 8;
    
    while (1) {
        clear_screen();
        printf("Found stations (Page %zu/%zu):\n", page + 1, (found.size() + PAGE_SIZE - 1) / PAGE_SIZE);
        
        size_t start = page * PAGE_SIZE;
        size_t end = std::min(start + PAGE_SIZE, found.size());

        for (size_t i = start; i < end; ++i) {
            printf("%zu. %s\n   --> %s\n", i + 1, found[i].name.c_str(), found[i].url.c_str());
        }
        printf("\n");
        if (end < found.size()) printf("n. Next page\n");
        if (page > 0) printf("p. Previous page\n");
        printf("q. To main menu\n");
        printf("Enter number to add, or navigation key: ");

        char input[10];
        if (!fgets(input, sizeof(input), stdin)) break;
        
        if (input[0] == 'n' || input[0] == 'N') {
            if (end < found.size()) page++;
        } else if (input[0] == 'p' || input[0] == 'P') {
            if (page > 0) page--;
        } else if (input[0] == 'q' || input[0] == 'Q') {
            break;
        } else if (isdigit(input[0])) {
            size_t choice = atoi(input);
            if (choice >= 1 && choice <= found.size()) {
                Station selected = found[choice - 1];

                if (!resolve_playlist_url(selected)) continue;

                // Re-check after resolving: a playlist can point at a format
                // the player cannot decode yet.
                if (is_unsupported_stream(selected.url)) {
                    printf("AAC is currently not supported\n");
                    wait_for_enter();
                    continue;
                }

                // Add station
                user_stations.push_back(selected);
                save_user_stations();
                printf("Added '%s' to your list.\n", selected.name.c_str());
                wait_for_enter();
                return; // Go back to main menu
            }
        }
    }
}

void add_station_manual() {
    clear_screen();
    printf("Add station manually\n");
    printf("====================\n\n");
    
    char name_buffer[256];
    printf("Enter station name: ");
    if (!fgets(name_buffer, sizeof(name_buffer), stdin)) return;
    name_buffer[strcspn(name_buffer, "\r\n")] = 0;
    if (strlen(name_buffer) == 0) return;

    char url_buffer[1024];
    printf("Enter URL: ");
    if (!fgets(url_buffer, sizeof(url_buffer), stdin)) return;
    url_buffer[strcspn(url_buffer, "\r\n")] = 0;
    if (strlen(url_buffer) == 0) return;

    Station selected;
    selected.name = name_buffer;
    selected.url = url_buffer;

    if (!resolve_playlist_url(selected)) return;

    if (is_unsupported_stream(selected.url)) {
        printf("AAC is currently not supported\n");
        wait_for_enter();
        return;
    }

    user_stations.push_back(selected);
    save_user_stations();
    printf("Added '%s' to your list.\n", selected.name.c_str());
    wait_for_enter();
}

void remove_station() {
    clear_screen();
    printf("Remove station\n");
    printf("==============\n\n");
    
    if (user_stations.empty()) {
        printf("(No stations to remove)\n");
        wait_for_enter();
        return;
    }

    for (size_t i = 0; i < user_stations.size(); ++i) {
        printf("%zu. %s\n", i + 1, user_stations[i].name.c_str());
    }
    printf("q. Cancel\n");
    printf("\nNumber to remove: ");

    char input[10];
    if (!fgets(input, sizeof(input), stdin)) return;
    
    if (input[0] == 'q' || input[0] == 'Q') return;

    if (isdigit(input[0])) {
        size_t choice = atoi(input);
        if (choice >= 1 && choice <= user_stations.size()) {
            printf("Removing '%s'\n", user_stations[choice - 1].name.c_str());
            user_stations.erase(user_stations.begin() + (choice - 1));
            save_user_stations();
            wait_for_enter();
        }
    }
}

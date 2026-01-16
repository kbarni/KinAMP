#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
void remove_station();
std::string to_lower(const std::string& str);
bool case_insensitive_contains(const std::string& str, const std::string& sub);

bool ends_with_ci(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(), 
        [](unsigned char a, unsigned char b){ return std::tolower(a) == std::tolower(b); });
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
    printf("Q - Quit\n\n");
    printf("Your choice: ");

    char choice[10];
    if (!fgets(choice, sizeof(choice), stdin)) return;

    switch (choice[0]) {
        case '1': list_stations(); break;
        case '2': add_station(); break;
        case '3': remove_station(); break;
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

std::vector<std::string> fetch_playlist_urls(const std::string& url) {
    std::vector<std::string> urls;
    std::string cmd = "wget -q -O - \"" + url + "\"";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return urls;

    char buffer[1024];
    bool is_pls = ends_with_ci(url, ".pls");

    while (fgets(buffer, sizeof(buffer), pipe)) {
        buffer[strcspn(buffer, "\r\n")] = 0; // trim newline
        std::string line = buffer;
        if (line.empty()) continue;

        if (is_pls) {
            // Check for FileX=url
            if (line.size() > 4 && std::tolower(line[0]) == 'f' && 
                                   std::tolower(line[1]) == 'i' && 
                                   std::tolower(line[2]) == 'l' && 
                                   std::tolower(line[3]) == 'e') {
                 size_t eq = line.find('=');
                 if (eq != std::string::npos) {
                     urls.push_back(line.substr(eq + 1));
                 }
            }
        } else {
            // M3U
            if (line[0] == '#') continue;
            urls.push_back(line);
        }
    }
    pclose(pipe);
    return urls;
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

                if (ends_with_ci(selected.url, ".aac") || ends_with_ci(selected.url, ".m3u8")) {
                    printf("AAC is currently not supported\n");
                    wait_for_enter();
                    continue; 
                }

                if (ends_with_ci(selected.url, ".m3u") || ends_with_ci(selected.url, ".pls")) {
                    printf("Downloading playlist...\n");
                    std::vector<std::string> streams = fetch_playlist_urls(selected.url);
                    if (streams.empty()) {
                        printf("No streams found in playlist.\n");
                        wait_for_enter();
                        continue;
                    }
                    
                    clear_screen();
                    printf("Select stream from playlist:\n");
                    for (size_t k = 0; k < streams.size(); ++k) {
                        printf("%zu. %s\n", k + 1, streams[k].c_str());
                    }
                    printf("c. Cancel\n");
                    printf("Choice: ");
                    char subinput[10];
                    if (fgets(subinput, sizeof(subinput), stdin)) {
                        if (subinput[0] == 'c' || subinput[0] == 'C') continue;
                        if (isdigit(subinput[0])) {
                            size_t subchoice = atoi(subinput);
                            if (subchoice >= 1 && subchoice <= streams.size()) {
                                selected.url = streams[subchoice - 1];
                            } else {
                                continue;
                            }
                        } else {
                            continue;
                        }
                    } else {
                        continue;
                    }
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

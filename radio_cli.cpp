#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <vector>
#include <string>
#include <algorithm>

#include "station_db.h"

// Function prototypes
std::string get_config_path(const char* filename);
void clear_screen();
void wait_for_enter();
void show_main_menu();
void list_stations();
void add_station();
void add_station_manual();
void remove_station();

std::vector<Station> user_stations;
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

// Asked by stationdb when a playlist holds more than one stream.
static bool cli_choose_stream(const std::vector<std::string>& streams, size_t& index, void*) {
    clear_screen();
    printf("Select stream from playlist:\n");
    for (size_t k = 0; k < streams.size(); ++k) {
        printf("%zu. %s%s\n", k + 1, streams[k].c_str(),
               stationdb::is_unsupported_stream(streams[k]) ? "  (unsupported)" : "");
    }
    printf("c. Cancel\n");
    printf("Choice: ");

    char subinput[10];
    if (!fgets(subinput, sizeof(subinput), stdin)) return false;
    if (!isdigit((unsigned char)subinput[0])) return false;
    size_t subchoice = atoi(subinput);
    if (subchoice < 1 || subchoice > streams.size()) return false;
    index = subchoice - 1;
    return true;
}

// Unwraps .pls/.m3u links down to a stream. False if the user cancelled or
// nothing usable turned up.
static bool cli_resolve_playlist_url(Station& station) {
    if (stationdb::needs_resolving(station.url)) {
        printf("Resolving playlist...\n");
        fflush(stdout);
    }

    std::string original = station.url;
    std::string error;
    stationdb::ResolveResult result =
        stationdb::resolve_playlist_url(station, cli_choose_stream, NULL, &error);

    if (result == stationdb::RESOLVE_CANCELLED) return false;
    if (result == stationdb::RESOLVE_ERROR) {
        printf("%s\n", error.c_str());
        wait_for_enter();
        return false;
    }
    if (station.url != original) printf("  -> %s\n", station.url.c_str());
    return true;
}

// A station is only added once it resolves to something the player can decode.
static bool accept_station(Station& station) {
    if (!cli_resolve_playlist_url(station)) return false;

    // Re-check after resolving: a playlist can point at a format the player
    // can't decode yet.
    if (stationdb::is_unsupported_stream(station.url)) {
        printf("HLS (.m3u8) streams are not supported yet\n");
        wait_for_enter();
        return false;
    }

    user_stations.push_back(station);
    save_user_stations();
    printf("Added '%s' to your list.\n", station.name.c_str());
    wait_for_enter();
    return true;
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
    if (!stationdb::search(term, found)) {
        printf("Error: Could not open stations database (allStations.json).\n");
        wait_for_enter();
        return;
    }

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
                if (accept_station(selected)) return; // Go back to main menu
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
    accept_station(selected);
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

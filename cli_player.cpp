#include <glib.h>
#include <gst/gst.h>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <fstream>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <memory>

#include "music_backend.h"

// Reuse the strategy enum
enum PlaybackStrategy {
    NORMAL,
    REPEAT,
    RANDOM
};

struct CliState {
    MusicBackend* backend;
    std::vector<std::string> playlist;
    std::vector<std::string> radio_urls;
    std::vector<std::string> radio_names;
    int current_index;
    PlaybackStrategy strategy;
    GMainLoop* loop;
    bool explicit_playlist; // True if playlist was passed as arg
    bool is_radio_mode;
};

// Global pointer for signal handling
static CliState* g_state = nullptr;

std::string get_config_path(const char* filename) {
    return std::string(filename);
}

// --- Helper: Load Playlist ---
bool load_playlist(const std::string& filepath, std::vector<std::string>& playlist) {
    std::ifstream infile(filepath.c_str());
    if (!infile.is_open()) return false;

    std::string line;
    while (std::getline(infile, line)) {
        if (!line.empty()) {
            // Basic trimming
            if (line[line.length()-1] == '\r') {
                line.erase(line.length()-1);
            }
            playlist.push_back(line);
        }
    }
    return true;
}

// --- Helper: Load Radio Stations ---
bool load_radio_stations(CliState* state) {
    std::string path = get_config_path(".kinamp_radio.txt");
    std::ifstream infile(path.c_str());
    state->radio_urls.clear();
    state->radio_names.clear();
    if (!infile.is_open()) return false;

    std::string line;
    while (std::getline(infile, line)) {
        size_t pos = line.find('|');
        if (pos != std::string::npos) {
            state->radio_names.push_back(line.substr(0, pos));
            state->radio_urls.push_back(line.substr(pos + 1));
        }
    }
    return true;
}

// --- Helper: Load Default Config (State) ---
void load_default_state(CliState* state) {
    std::string config_path = get_config_path(".kinamp.conf");
    std::ifstream conffile(config_path.c_str());
    if (conffile.is_open()) {
        std::string line;
        while (std::getline(conffile, line)) {
            if (line.find("current_index=") == 0) {
                state->current_index = atoi(line.substr(14).c_str());
            }
            if (line.find("playback_strategy=") == 0) {
                int strat = atoi(line.substr(18).c_str());
                state->strategy = (PlaybackStrategy)strat;
            }
            if (line.find("is_radio_mode=") == 0) {
                state->is_radio_mode = (atoi(line.substr(14).c_str()) != 0);
            }
        }
        conffile.close();
    }
}

// --- Logic: Play Next ---
void play_next(CliState* state) {
    size_t total_items = state->is_radio_mode ? state->radio_urls.size() : state->playlist.size();
    
    if (total_items == 0) {
        g_print("List is empty.\n");
        g_main_loop_quit(state->loop);
        return;
    }

    int next_index = -1;

    switch (state->strategy) {
        case NORMAL:
            if (state->current_index + 1 < (int)total_items) {
                next_index = state->current_index + 1;
            } else {
                g_print("End of list reached.\n");
                g_main_loop_quit(state->loop);
                return;
            }
            break;
        case REPEAT:
            if (state->current_index + 1 < (int)total_items) {
                next_index = state->current_index + 1;
            } else {
                next_index = 0;
            }
            break;
        case RANDOM: {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(0, total_items - 1);
            next_index = distrib(gen);
            break;
        }
    }

    if (next_index >= 0) {
        state->current_index = next_index;
        if (state->is_radio_mode) {
            std::string url = state->radio_urls[next_index];
            std::string name = state->radio_names[next_index];
            g_print("Playing Radio [%d/%zu]: %s (%s)\n", next_index + 1, total_items, name.c_str(), url.c_str());
            state->backend->play_file(url.c_str());
        } else {
            std::string file = state->playlist[next_index];
            g_print("Playing [%d/%zu]: %s\n", next_index + 1, total_items, file.c_str());
            state->backend->play_file(file.c_str());
        }
    }
}

// --- Callback: End Of Stream ---
void on_eos_callback(void* user_data) {
    CliState* state = (CliState*)user_data;
    if (state->is_radio_mode) {
        g_print("Radio stream ended. Reconnecting in 5 seconds...\n");
        sleep(5);
        std::string url = state->radio_urls[state->current_index];
        state->backend->play_file(url.c_str());
    } else {
        play_next(state);
    }
}

// --- Signal Handler ---
void handle_sigint(int sig) {
    (void)sig;
    if (g_state) {
        g_print("\nStopping...\n");
        g_state->backend->stop();
        g_main_loop_quit(g_state->loop);
    }
}

int main(int argc, char* argv[]) {
    MusicBackend backend;
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);

    CliState state;
    state.backend = &backend;
    state.loop = loop;
    state.current_index = -1;
    state.strategy = NORMAL; 
    state.explicit_playlist = false;
    state.is_radio_mode = false;
    g_state = &state;

    // 2. Parse Arguments
    std::string playlist_arg;
    bool strategy_overridden = false;
    bool radio_overridden = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--repeat") {
            state.strategy = REPEAT;
            strategy_overridden = true;
        } else if (arg == "--shuffle") {
            state.strategy = RANDOM;
            strategy_overridden = true;
        } else if (arg == "--radio") {
            state.is_radio_mode = true;
            radio_overridden = true;
        } else if (arg == "--music") {
            state.is_radio_mode = false;
            radio_overridden = true;
        } else if (arg[0] != '-') {
            playlist_arg = arg;
            state.explicit_playlist = true;
        }
    }

    // 3. Load Configuration/Playlist
    if (state.explicit_playlist) {
        if (state.is_radio_mode) {
            state.radio_urls.clear();
            state.radio_names.clear();
            state.radio_urls.push_back(playlist_arg);
            state.radio_names.push_back("Custom Stream");
            state.current_index = -1;
        } else {
            if (!load_playlist(playlist_arg, state.playlist)) {
                g_printerr("Error: Could not load playlist '%s'\n", playlist_arg.c_str());
                return 1;
            }
            state.current_index = -1;
            state.is_radio_mode = false;
        }
    } else {
        CliState saved_state;
        saved_state.current_index = 0;
        saved_state.strategy = NORMAL;
        saved_state.is_radio_mode = false;
        load_default_state(&saved_state);

        if (!radio_overridden) {
            state.is_radio_mode = saved_state.is_radio_mode;
        }

        if (state.is_radio_mode) {
            if (!load_radio_stations(&state)) {
                 g_printerr("Error: Could not load radio stations.\n");
                 return 1;
            }
        } else {
            std::string default_pl = get_config_path(".kinamp_playlist.m3u");
            if (!load_playlist(default_pl, state.playlist)) {
                g_printerr("Error: Could not load default playlist '%s'\n", default_pl.c_str());
                return 1;
            }
        }
        
        state.current_index = saved_state.current_index - 1; 
        if (!strategy_overridden) {
            state.strategy = saved_state.strategy;
        }
    }

    if (!state.is_radio_mode && state.playlist.empty()) {
        g_printerr("Error: Playlist is empty.\n");
        return 1;
    }
    if (state.is_radio_mode && state.radio_urls.empty()) {
        g_printerr("Error: Radio station list is empty.\n");
        return 1;
    }

    // 4. Setup Signal Handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    // 5. Start Playback
    backend.set_eos_callback(on_eos_callback, &state);

    g_print("KinAMP-minimal started.\n");
    if (state.is_radio_mode) {
        g_print("Mode: Radio\n");
        g_print("Radio list size: %zu\n", state.radio_urls.size());
    } else {
        g_print("Mode: Music\n");
        g_print("Playlist size: %zu\n", state.playlist.size());
    }
    g_print("Strategy: %s\n", state.strategy == NORMAL ? "Normal" : (state.strategy == REPEAT ? "Repeat" : "Shuffle"));

    // Kick off the first item
    play_next(&state);

    // 6. Run Loop
    g_main_loop_run(loop);

    // 7. Cleanup
    g_main_loop_unref(loop);
    
    return 0;
}
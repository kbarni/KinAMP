#include <glib.h>
#include <gst/gst.h>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <fstream>
#include <wordexp.h>
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
    int current_index;
    PlaybackStrategy strategy;
    GMainLoop* loop;
    bool explicit_playlist; // True if playlist was passed as arg
};

// Global pointer for signal handling
static CliState* g_state = nullptr;

std::string get_config_path(const char* filename) {
    return filename;
}

// --- Helper: Load Playlist ---
bool load_playlist(const std::string& filepath, std::vector<std::string>& playlist) {
    std::ifstream infile(filepath.c_str());
    if (!infile.is_open()) return false;

    std::string line;
    while (std::getline(infile, line)) {
        if (!line.empty()) {
            // Basic trimming could be added here if needed
            if (line[line.length()-1] == '\r') {
                line.erase(line.length()-1);
            }
            playlist.push_back(line);
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
        }
        conffile.close();
    }
}

// --- Logic: Play Next ---
void play_next(CliState* state) {
    if (state->playlist.empty()) {
        g_print("Playlist is empty.\n");
        g_main_loop_quit(state->loop);
        return;
    }

    int next_index = -1;

    switch (state->strategy) {
        case NORMAL:
            if (state->current_index + 1 < (int)state->playlist.size()) {
                next_index = state->current_index + 1;
            } else {
                // End of playlist
                g_print("End of playlist reached.\n");
                g_main_loop_quit(state->loop);
                return;
            }
            break;
        case REPEAT:
            if (state->current_index + 1 < (int)state->playlist.size()) {
                next_index = state->current_index + 1;
            } else {
                // Loop back to start
                next_index = 0;
            }
            break;
        case RANDOM: {
            // Simple random pick
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(0, state->playlist.size() - 1);
            next_index = distrib(gen);
            break;
        }
    }

    if (next_index >= 0) {
        state->current_index = next_index;
        std::string file = state->playlist[next_index];
        g_print("Playing [%d/%zu]: %s\n", next_index + 1, state->playlist.size(), file.c_str());
        state->backend->play_file(file.c_str());
    }
}

// --- Callback: End Of Stream ---
void on_eos_callback(void* user_data) {
    CliState* state = (CliState*)user_data;
    // Backend has already stopped playback of current track.
    // Trigger next song.
    play_next(state);
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
    // 1. Setup GMainLoop and Backend
    // Note: MusicBackend calls gst_init(NULL, NULL).
    MusicBackend backend;
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);

    CliState state;
    state.backend = &backend;
    state.loop = loop;
    state.current_index = -1;
    state.strategy = NORMAL; // Default
    state.explicit_playlist = false;
    g_state = &state;

    // 2. Parse Arguments
    std::string playlist_arg;
    bool strategy_overridden = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--repeat") {
            state.strategy = REPEAT;
            strategy_overridden = true;
        } else if (arg == "--shuffle") {
            state.strategy = RANDOM;
            strategy_overridden = true;
        } else if (arg[0] != '-') {
            playlist_arg = arg;
            state.explicit_playlist = true;
        }
    }

    // 3. Load Configuration/Playlist
    if (state.explicit_playlist) {
        if (!load_playlist(playlist_arg, state.playlist)) {
            g_printerr("Error: Could not load playlist '%s'\n", playlist_arg.c_str());
            return 1;
        }
        // When explicit playlist is used, start from 0 unless logic changes.
        state.current_index = -1; 
    } else {
        // Load default config
        std::string default_pl = get_config_path(".kinamp_playlist.m3u");
        if (!load_playlist(default_pl, state.playlist)) {
            g_printerr("Error: Could not load default playlist '%s'\n", default_pl.c_str());
            return 1;
        }
        
        // Load previous state (index, strategy)
        // Only load strategy if not overridden by CLI flags
        CliState saved_state;
        saved_state.current_index = 0;
        saved_state.strategy = NORMAL;
        load_default_state(&saved_state);

        state.current_index = saved_state.current_index - 1; // -1 because play_next increments
        if (!strategy_overridden) {
            state.strategy = saved_state.strategy;
        }
    }

    if (state.playlist.empty()) {
        g_printerr("Error: Playlist is empty.\n");
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
    g_print("Playlist size: %zu\n", state.playlist.size());
    g_print("Strategy: %s\n", state.strategy == NORMAL ? "Normal" : (state.strategy == REPEAT ? "Repeat" : "Shuffle"));

    // Kick off the first song
    play_next(&state);

    // 6. Run Loop
    g_main_loop_run(loop);

    // 7. Cleanup
    g_main_loop_unref(loop);
    
    return 0;
}

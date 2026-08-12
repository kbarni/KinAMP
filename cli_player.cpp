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
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "music_backend.h"

#ifdef KINAMP_HAVE_LIPC
#include "openlipc/openlipc.h"
#endif

// Reuse the strategy enum
enum PlaybackStrategy {
    NORMAL,
    REPEAT,
    RANDOM
};

// Control channel: a FIFO we read newline-terminated commands from, and a status
// file we rewrite whenever something changes. Clients that cannot open the FIFO
// know no player is running.
//
// These do NOT live next to the config files. The install directory is on
// /mnt/us, which is vfat: it cannot represent a FIFO at all, so mkfifo() there
// fails with EPERM and the player ends up running with no way to be controlled.
// Rewriting a status file once a second on that flash would be unkind too.
// /tmp is a real filesystem on the device - music_backend.cpp already puts its
// audio pipe there. KINAMP_RUNTIME_DIR overrides it.
#define CMD_FIFO_NAME    "kinamp_cmd"
#define STATUS_FILE_NAME "kinamp_status"
#define COVER_FILE_STEM  "kinamp_cover"

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

    // --- Control channel ---
    bool daemon_mode;        // stay alive when the playlist ends
    int cmd_fd;              // read end of the command FIFO (-1 if unavailable)
    GIOChannel* cmd_channel;
    guint cmd_watch_id;
    std::string cmd_partial; // carries an unterminated line between reads
    guint status_timer_id;   // 1 Hz position refresh, only while playing
    guint reconnect_timer_id;// pending radio reconnect
    std::string cover_path;  // written on track change, "" if no artwork
    bool stopped;            // explicitly stopped, as opposed to never started
};

// Global pointer for signal handling
static CliState* g_state = nullptr;

// Config and playlist files are relative to the working directory, which the
// launchers set to the install directory: they have to persist across reboots.
std::string get_config_path(const char* filename) {
    return std::string(filename);
}

// Runtime files (command FIFO, status, cover art) live on a filesystem that can
// actually hold them - see the note on CMD_FIFO_NAME above.
std::string get_runtime_path(const char* filename) {
    const char* dir = getenv("KINAMP_RUNTIME_DIR");
    if (!dir || !*dir) dir = "/tmp";
    std::string path(dir);
    if (!path.empty() && path[path.size() - 1] != '/') path += '/';
    return path + filename;
}

static void write_status(CliState* state);
static void play_index(CliState* state, int index);

// --- Bluetooth keepalive ---
// The Kindle drops an idle Bluetooth audio device after 20 minutes.
// com.lab126.btfd's ensureBTconnection flag inhibits that. The GTK player raises
// it at startup and lowers it in quit_app(), but on_background_clicked() hands
// playback over to us with the flag still raised and nothing left running that
// would ever lower it again - so the device held the link alive long after the
// music stopped. We therefore own the flag outright for our own lifetime.
//
// Deliberately scoped to the process, not to the track: raised lazily on the
// first thing we play and held until we exit, never lowered in between. Under
// the KOReader plugin, where a --daemon instance spans the whole session, that
// is one write per session rather than one per track.
//
// Note what is NOT here: any write to BTenable. The stock keepalive sequence
// cycles the radio (BTenable 0:1 -> flag -> 1:1), which disconnects the
// headphones for several seconds and makes most devices chime on reconnect;
// repeating that per track would be audible every time. We only ever write the
// flag, which disturbs nothing.
//
// That does mean this write alone arms nothing: btfd reads ensureBTconnection
// when the radio comes up and never again, so it has to be raised into a gap
// somebody else opens. Both launch paths open one before we exist -
// startkinamp.sh takes the radio down ahead of the GTK player, and the KOReader
// plugin cycles it in kinamp_bt.lua's armKeepalive() before starting us. What
// our own write is for is the other end: lowering the flag on exit, so the next
// radio init does not inherit a keepalive nobody asked for, and raising it again
// for any cycle that happens while we are playing.
//
// liblipc only exists on the Kindle; on the desktop host these are no-ops.
#ifdef KINAMP_HAVE_LIPC

#define BTFD_SERVICE      "com.lab126.btfd"
#define BT_KEEPALIVE_PROP "ensureBTconnection"

static LIPC* lipc_instance = NULL;
static bool bt_keepalive_held = false;

static void acquire_bt_keepalive() {
    if (bt_keepalive_held) return;

    if (lipc_instance == NULL) {
        // No service name: we only write another service's property, so there is
        // nothing to register on the bus. That keeps us clear of KOReader's own
        // lipc handles and of a second instance of ourselves.
        lipc_instance = LipcOpenNoName();
        if (lipc_instance == NULL) {
            g_printerr("Warning: could not open LIPC, Bluetooth keepalive unavailable.\n");
            return;
        }
    }

    LIPCcode code = LipcSetIntProperty(lipc_instance, BTFD_SERVICE, BT_KEEPALIVE_PROP, 1);
    if (code != LIPC_OK) {
        // Left unheld, so the next track retries. Worth reporting: silently
        // losing the keepalive looks like a firmware bug 20 minutes later.
        g_printerr("Warning: could not set %s (lipc code %d).\n", BT_KEEPALIVE_PROP, (int)code);
        return;
    }
    bt_keepalive_held = true;
    g_print("Bluetooth keepalive on.\n");
}

static void release_bt_keepalive() {
    if (lipc_instance == NULL) return;
    if (bt_keepalive_held) {
        LipcSetIntProperty(lipc_instance, BTFD_SERVICE, BT_KEEPALIVE_PROP, 0);
        bt_keepalive_held = false;
    }
    LipcClose(lipc_instance);
    lipc_instance = NULL;
}

#else

static void acquire_bt_keepalive() {}
static void release_bt_keepalive() {}

#endif

// --- Helper: Load Playlist ---
// Decides whether an m3u line names a track, and cleans it up in place.
// Playlists written elsewhere carry #EXTM3U/#EXTINF directives, which are not
// paths, and often use CRLF line endings, whose trailing CR would otherwise
// become part of the file name and make every entry unopenable.
// (music_player.cpp has the same helper; the CLI player never got it.)
static bool m3u_entry(std::string* line) {
    while (!line->empty() && (*line->rbegin() == '\r' || *line->rbegin() == '\n')) {
        line->erase(line->size() - 1);
    }
    return !line->empty() && (*line)[0] != '#';
}

bool load_playlist(const std::string& filepath, std::vector<std::string>& playlist) {
    std::ifstream infile(filepath.c_str());
    if (!infile.is_open()) return false;

    // Relative entries in an m3u are relative to the playlist's own directory,
    // not to our working directory (the install dir), so they have to be
    // rebased or nothing in a playlist saved next to the music will open.
    std::string base;
    size_t slash = filepath.find_last_of('/');
    if (slash != std::string::npos) {
        base = filepath.substr(0, slash + 1);
    }

    std::string line;
    while (std::getline(infile, line)) {
        if (!m3u_entry(&line)) continue;

        bool is_absolute = line[0] == '/';
        bool is_url = line.find("://") != std::string::npos;
        if (!is_absolute && !is_url && !base.empty()) {
            line = base + line;
        }
        playlist.push_back(line);
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

// --- Helper: Persist current_index so the GTK player resumes where we stopped ---
// The GUI writes .kinamp.conf on exit and startkinamp.sh hands playback over to
// us; without this write-back the handover only works in one direction.
void save_current_index(CliState* state) {
    if (state->is_radio_mode) return;

    std::string config_path = get_config_path(".kinamp.conf");
    std::vector<std::string> lines;
    bool replaced = false;

    std::ifstream infile(config_path.c_str());
    if (infile.is_open()) {
        std::string line;
        while (std::getline(infile, line)) {
            if (line.find("current_index=") == 0) {
                lines.push_back("current_index=" + std::to_string(state->current_index));
                replaced = true;
            } else {
                lines.push_back(line);
            }
        }
        infile.close();
    }
    if (!replaced) {
        lines.push_back("current_index=" + std::to_string(state->current_index));
    }

    std::string tmp_path = config_path + ".tmp";
    std::ofstream outfile(tmp_path.c_str());
    if (!outfile.is_open()) return;
    for (size_t i = 0; i < lines.size(); ++i) outfile << lines[i] << "\n";
    outfile.close();
    rename(tmp_path.c_str(), config_path.c_str());
}

// --- Helper: Dump embedded cover art so the UI can display it ---
void write_cover_art(CliState* state) {
    if (!state->cover_path.empty()) {
        unlink(state->cover_path.c_str());
        state->cover_path.clear();
    }

    const std::vector<unsigned char>& art = state->backend->cover_art;
    if (art.size() < 4) return;

    const char* ext = NULL;
    if (art[0] == 0xFF && art[1] == 0xD8) {
        ext = ".jpg";
    } else if (art[0] == 0x89 && art[1] == 'P' && art[2] == 'N' && art[3] == 'G') {
        ext = ".png";
    } else {
        return; // unknown container, don't hand the UI something it can't decode
    }

    std::string path = get_runtime_path(COVER_FILE_STEM) + ext;
    std::string tmp_path = path + ".tmp";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    if (!f) return;
    size_t written = fwrite(&art[0], 1, art.size(), f);
    fclose(f);
    if (written != art.size()) {
        unlink(tmp_path.c_str());
        return;
    }
    if (rename(tmp_path.c_str(), path.c_str()) == 0) {
        state->cover_path = path;
    } else {
        unlink(tmp_path.c_str());
    }
}

// --- Helper: Status file ---
static std::string sanitize(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
    }
    return out;
}

static gboolean status_tick(gpointer data) {
    write_status((CliState*)data);
    return TRUE;
}

// Only run the 1 Hz refresh while something is actually playing: a paused or
// stopped player has nothing to report and this is a battery-powered device.
static void sync_status_timer(CliState* state) {
    bool want_timer = state->backend->is_playing && !state->backend->is_paused;
    if (want_timer && state->status_timer_id == 0) {
        state->status_timer_id = g_timeout_add_seconds(1, status_tick, state);
    } else if (!want_timer && state->status_timer_id != 0) {
        g_source_remove(state->status_timer_id);
        state->status_timer_id = 0;
    }
}

static void write_status(CliState* state) {
    std::string path = get_runtime_path(STATUS_FILE_NAME);
    std::string tmp_path = path + ".tmp";

    std::ofstream out(tmp_path.c_str());
    if (!out.is_open()) return;

    const char* play_state = "stopped";
    if (state->backend->is_playing) {
        play_state = state->backend->is_paused ? "paused" : "playing";
    }

    size_t count = state->is_radio_mode ? state->radio_urls.size() : state->playlist.size();
    std::string current;
    std::string name;
    if (state->current_index >= 0 && state->current_index < (int)count) {
        if (state->is_radio_mode) {
            current = state->radio_urls[state->current_index];
            name = state->radio_names[state->current_index];
        } else {
            current = state->playlist[state->current_index];
        }
    }

    out << "pid=" << (int)getpid() << "\n";
    out << "state=" << play_state << "\n";
    out << "mode=" << (state->is_radio_mode ? "radio" : "music") << "\n";
    out << "daemon=" << (state->daemon_mode ? 1 : 0) << "\n";
    out << "index=" << state->current_index << "\n";
    out << "count=" << count << "\n";
    out << "strategy=" << (int)state->strategy << "\n";
    out << "path=" << sanitize(current) << "\n";
    out << "station=" << sanitize(name) << "\n";
    out << "title=" << sanitize(state->backend->meta_title) << "\n";
    out << "artist=" << sanitize(state->backend->meta_artist) << "\n";
    out << "album=" << sanitize(state->backend->meta_album) << "\n";
    out << "cover=" << state->cover_path << "\n";
    out << "pos=" << (long long)(state->backend->get_position() / GST_SECOND) << "\n";
    out << "dur=" << (long long)(state->backend->get_duration() / GST_SECOND) << "\n";
    out << "vol=" << (int)(state->backend->get_volume() * 100.0 + 0.5) << "\n";
    out.close();

    rename(tmp_path.c_str(), path.c_str());
}

// --- Callback: ICY now-playing title for radio streams ---
void on_metadata_callback(const char* title, void* user_data) {
    CliState* state = (CliState*)user_data;
    if (title) state->backend->meta_title = title;
    write_status(state);
}

// --- Logic: Play a specific index ---
static void play_index(CliState* state, int index) {
    size_t total_items = state->is_radio_mode ? state->radio_urls.size() : state->playlist.size();
    if (total_items == 0 || index < 0 || index >= (int)total_items) return;

    state->current_index = index;
    state->stopped = false;

    // Every fresh start funnels through here (seeks and radio reconnects go
    // straight to the backend, and by then we already hold it).
    acquire_bt_keepalive();

    if (state->is_radio_mode) {
        const std::string& url = state->radio_urls[index];
        g_print("Playing Radio [%d/%zu]: %s (%s)\n", index + 1, total_items,
                state->radio_names[index].c_str(), url.c_str());
        state->backend->meta_title.clear();
        state->backend->play_file(url.c_str());
    } else {
        const std::string& file = state->playlist[index];
        g_print("Playing [%d/%zu]: %s\n", index + 1, total_items, file.c_str());
        state->backend->play_file(file.c_str());
        save_current_index(state);
    }

    write_cover_art(state);
    sync_status_timer(state);
    write_status(state);
}

// --- Logic: Play Next ---
void play_next(CliState* state) {
    size_t total_items = state->is_radio_mode ? state->radio_urls.size() : state->playlist.size();

    if (total_items == 0) {
        g_print("List is empty.\n");
        if (!state->daemon_mode) g_main_loop_quit(state->loop);
        return;
    }

    int next_index = -1;

    switch (state->strategy) {
        case NORMAL:
            if (state->current_index + 1 < (int)total_items) {
                next_index = state->current_index + 1;
            } else {
                g_print("End of list reached.\n");
                // A daemon stays available for the next command instead of
                // exiting, which is what the KOReader plugin drives.
                if (state->daemon_mode) {
                    state->stopped = true;
                    sync_status_timer(state);
                    write_status(state);
                } else {
                    g_main_loop_quit(state->loop);
                }
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
        play_index(state, next_index);
    }
}

void play_prev(CliState* state) {
    size_t total_items = state->is_radio_mode ? state->radio_urls.size() : state->playlist.size();
    if (total_items == 0) return;

    int prev_index = state->current_index - 1;
    if (prev_index < 0) {
        prev_index = (state->strategy == NORMAL) ? 0 : (int)total_items - 1;
    }
    play_index(state, prev_index);
}

// --- Callback: End Of Stream ---
static gboolean radio_reconnect(gpointer data) {
    CliState* state = (CliState*)data;
    state->reconnect_timer_id = 0;
    if (state->stopped) return FALSE; // a stop command arrived while we waited
    if (state->current_index >= 0 && state->current_index < (int)state->radio_urls.size()) {
        state->backend->play_file(state->radio_urls[state->current_index].c_str());
        sync_status_timer(state);
        write_status(state);
    }
    return FALSE;
}

void on_eos_callback(void* user_data) {
    CliState* state = (CliState*)user_data;
    if (state->is_radio_mode) {
        // Scheduled rather than a blocking sleep(5), so commands stay responsive
        // while we wait to reconnect.
        g_print("Radio stream ended. Reconnecting in 5 seconds...\n");
        if (state->reconnect_timer_id == 0) {
            state->reconnect_timer_id = g_timeout_add_seconds(5, radio_reconnect, state);
        }
        sync_status_timer(state);
        write_status(state);
    } else {
        play_next(state);
    }
}

// --- Control channel: command handling ---
static void cancel_reconnect(CliState* state) {
    if (state->reconnect_timer_id != 0) {
        g_source_remove(state->reconnect_timer_id);
        state->reconnect_timer_id = 0;
    }
}

static void do_stop(CliState* state) {
    cancel_reconnect(state);
    state->stopped = true;
    state->backend->stop();
    sync_status_timer(state);
    write_status(state);
}

static void do_quit(CliState* state) {
    cancel_reconnect(state);
    state->backend->stop();
    g_main_loop_quit(state->loop);
}

void handle_command(CliState* state, const std::string& raw) {
    // Split "verb argument"; the argument keeps any embedded spaces.
    std::string line = raw;
    while (!line.empty() && (line[line.size()-1] == '\r' || line[line.size()-1] == ' ')) {
        line.erase(line.size()-1);
    }
    if (line.empty()) return;

    std::string verb = line;
    std::string arg;
    size_t sp = line.find(' ');
    if (sp != std::string::npos) {
        verb = line.substr(0, sp);
        arg = line.substr(sp + 1);
    }

    g_print("Command: %s%s%s\n", verb.c_str(), arg.empty() ? "" : " ", arg.c_str());

    if (verb == "pause" || verb == "toggle") {
        if (state->backend->is_playing) {
            state->backend->pause();
        } else if (state->current_index >= 0) {
            play_index(state, state->current_index); // resume after a stop
            return;
        }
        sync_status_timer(state);
        write_status(state);
    } else if (verb == "play") {
        if (state->backend->is_paused) {
            state->backend->pause(); // pause() toggles
            sync_status_timer(state);
            write_status(state);
        } else if (!state->backend->is_playing) {
            play_index(state, state->current_index >= 0 ? state->current_index : 0);
        }
    } else if (verb == "next") {
        cancel_reconnect(state);
        play_next(state);
    } else if (verb == "prev") {
        cancel_reconnect(state);
        play_prev(state);
    } else if (verb == "stop") {
        do_stop(state);
    } else if (verb == "quit") {
        do_quit(state);
    } else if (verb == "index") {
        cancel_reconnect(state);
        play_index(state, atoi(arg.c_str()));
    } else if (verb == "seek") {
        // MusicBackend has no seek; restarting the file at an offset is the only
        // mechanism available, and it is what the GTK player uses too.
        if (!state->is_radio_mode && state->current_index >= 0 &&
            state->current_index < (int)state->playlist.size()) {
            int seconds = atoi(arg.c_str());
            if (seconds < 0) seconds = 0;
            state->backend->play_file(state->playlist[state->current_index].c_str(), seconds);
            sync_status_timer(state);
            write_status(state);
        }
    } else if (verb == "vol") {
        int pct = atoi(arg.c_str());
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        state->backend->set_volume(pct / 100.0);
        write_status(state);
    } else if (verb == "strategy") {
        int s = atoi(arg.c_str());
        if (s >= NORMAL && s <= RANDOM) state->strategy = (PlaybackStrategy)s;
        write_status(state);
    } else if (verb == "load") {
        // Stages the playlist without starting it, so a client can write
        // "load <path>\nindex <n>" in one go and land directly on the track it
        // wants instead of briefly playing the first one.
        std::vector<std::string> loaded;
        if (!arg.empty() && load_playlist(arg, loaded) && !loaded.empty()) {
            cancel_reconnect(state);
            state->backend->stop();
            // swap, not assign: taking ownership of the buffer avoids copying
            // every path, and keeps this out of the allocator's growth path.
            state->playlist.swap(loaded);
            state->is_radio_mode = false;
            state->current_index = -1;
            state->stopped = true;
            sync_status_timer(state);
            write_status(state);
        } else {
            g_printerr("Command: could not load playlist '%s'\n", arg.c_str());
        }
    } else if (verb == "radio") {
        if (!arg.empty()) {
            cancel_reconnect(state);
            state->is_radio_mode = true;
            state->radio_urls.clear();
            state->radio_names.clear();
            state->radio_urls.push_back(arg);
            state->radio_names.push_back("Custom Stream");
            state->current_index = -1;
            play_index(state, 0);
        }
    } else if (verb == "status") {
        write_status(state);
    } else {
        g_printerr("Command: unknown verb '%s'\n", verb.c_str());
    }
}

static gboolean on_cmd_readable(GIOChannel* source, GIOCondition condition, gpointer data) {
    (void)source;
    CliState* state = (CliState*)data;

    if (condition & (G_IO_ERR | G_IO_NVAL)) {
        return TRUE;
    }

    char buf[1024];
    ssize_t n;
    while ((n = read(state->cmd_fd, buf, sizeof(buf))) > 0) {
        state->cmd_partial.append(buf, (size_t)n);
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        g_printerr("Command FIFO read error: %s\n", strerror(errno));
    }

    size_t nl;
    while ((nl = state->cmd_partial.find('\n')) != std::string::npos) {
        std::string line = state->cmd_partial.substr(0, nl);
        state->cmd_partial.erase(0, nl + 1);
        handle_command(state, line);
        // handle_command may have quit the loop; stop touching state afterwards.
        if (!g_main_loop_is_running(state->loop)) break;
    }
    // Guard against a client that never sends a newline.
    if (state->cmd_partial.size() > 4096) state->cmd_partial.clear();

    return TRUE;
}

static bool setup_command_fifo(CliState* state) {
    std::string path = get_runtime_path(CMD_FIFO_NAME);

    // A stale FIFO from a killed instance is harmless, but a stale *regular file*
    // of the same name would silently swallow every command, so always recreate.
    unlink(path.c_str());
    if (mkfifo(path.c_str(), 0666) == -1 && errno != EEXIST) {
        // Worth spelling out: without the FIFO the player still plays, but it
        // cannot be controlled, and every client sees "no player running" while
        // clearly hearing one. EPERM here almost always means the directory is
        // on a filesystem that cannot hold a FIFO, i.e. vfat.
        g_printerr("Could not create command FIFO '%s': %s\n", path.c_str(), strerror(errno));
        g_printerr("  Remote control is disabled: the player will play but cannot be\n"
                   "  paused, skipped or stopped, and clients will report no player\n"
                   "  running. '%s' may be on a filesystem that cannot hold a FIFO\n"
                   "  (vfat gives EPERM here). Set KINAMP_RUNTIME_DIR to a directory\n"
                   "  on a real filesystem.\n",
                   path.substr(0, path.find_last_of('/') + 1).c_str());
        return false;
    }

    // O_RDWR keeps a writer on the FIFO at all times. With a read-only fd the
    // watch would fire continuously with G_IO_HUP each time a client closes.
    state->cmd_fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (state->cmd_fd < 0) {
        g_printerr("Could not open command FIFO '%s': %s\n", path.c_str(), strerror(errno));
        return false;
    }

    state->cmd_channel = g_io_channel_unix_new(state->cmd_fd);
    g_io_channel_set_encoding(state->cmd_channel, NULL, NULL);
    g_io_channel_set_buffered(state->cmd_channel, FALSE);
    state->cmd_watch_id = g_io_add_watch(state->cmd_channel,
                                         (GIOCondition)(G_IO_IN | G_IO_ERR),
                                         on_cmd_readable, state);
    g_print("Command FIFO: %s\n", path.c_str());
    return true;
}

static void teardown_control_channel(CliState* state) {
    if (state->cmd_watch_id != 0) {
        g_source_remove(state->cmd_watch_id);
        state->cmd_watch_id = 0;
    }
    if (state->cmd_channel) {
        g_io_channel_unref(state->cmd_channel);
        state->cmd_channel = NULL;
    }
    if (state->cmd_fd >= 0) {
        close(state->cmd_fd);
        state->cmd_fd = -1;
    }
    if (state->status_timer_id != 0) {
        g_source_remove(state->status_timer_id);
        state->status_timer_id = 0;
    }
    cancel_reconnect(state);
    release_bt_keepalive();

    unlink(get_runtime_path(CMD_FIFO_NAME).c_str());
    unlink(get_runtime_path(STATUS_FILE_NAME).c_str());
    if (!state->cover_path.empty()) unlink(state->cover_path.c_str());
}

// --- Signal Handler ---
// SIGTERM matters as much as SIGINT here: both launcher scripts stop background
// playback with a plain pkill, and without this the FIFO and status file would
// be left behind for the next instance to trip over.
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
    state.daemon_mode = false;
    state.cmd_fd = -1;
    state.cmd_channel = NULL;
    state.cmd_watch_id = 0;
    state.status_timer_id = 0;
    state.reconnect_timer_id = 0;
    state.stopped = false;
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
        } else if (arg == "--daemon") {
            // Stay alive when the list ends and accept commands indefinitely.
            // Only the KOReader launcher passes this; startkinamp.sh relies on
            // the process exiting so the booklet toggles back to the GUI.
            state.daemon_mode = true;
        } else if (arg == "--idle") {
            // Daemon that starts with nothing playing, waiting for commands.
            state.daemon_mode = true;
            state.stopped = true;
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

        // A daemon that finds nothing to play still starts up and waits for a
        // "load" or "radio" command; a one-shot run has nothing to do and exits.
        if (state.is_radio_mode) {
            if (!load_radio_stations(&state)) {
                 g_printerr("Error: Could not load radio stations.\n");
                 if (!state.daemon_mode) return 1;
            }
        } else {
            std::string default_pl = get_config_path(".kinamp_playlist.m3u");
            if (!load_playlist(default_pl, state.playlist)) {
                g_printerr("Error: Could not load default playlist '%s'\n", default_pl.c_str());
                if (!state.daemon_mode) return 1;
            }
        }
        
        state.current_index = saved_state.current_index - 1; 
        if (!strategy_overridden) {
            state.strategy = saved_state.strategy;
        }
    }

    // A daemon is allowed to start with nothing loaded; it waits for a command.
    if (!state.daemon_mode) {
        if (!state.is_radio_mode && state.playlist.empty()) {
            g_printerr("Error: Playlist is empty.\n");
            return 1;
        }
        if (state.is_radio_mode && state.radio_urls.empty()) {
            g_printerr("Error: Radio station list is empty.\n");
            return 1;
        }
    }

    // 4. Setup Signal Handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 5. Start Playback
    backend.set_eos_callback(on_eos_callback, &state);
    backend.set_metadata_callback(on_metadata_callback, &state);

    g_print("KinAMP-minimal started.\n");
    if (state.is_radio_mode) {
        g_print("Mode: Radio\n");
        g_print("Radio list size: %zu\n", state.radio_urls.size());
    } else {
        g_print("Mode: Music\n");
        g_print("Playlist size: %zu\n", state.playlist.size());
    }
    g_print("Strategy: %s\n", state.strategy == NORMAL ? "Normal" : (state.strategy == REPEAT ? "Repeat" : "Shuffle"));
    if (state.daemon_mode) g_print("Daemon mode: on\n");

    // 5b. Control channel. Playback still works without it, so a failure here is
    // reported but not fatal.
    setup_command_fifo(&state);
    write_status(&state);

    // Kick off the first item
    if (!state.stopped) {
        play_next(&state);
    }

    // 6. Run Loop
    g_main_loop_run(loop);

    // 7. Cleanup
    teardown_control_channel(&state);
    g_main_loop_unref(loop);

    return 0;
}
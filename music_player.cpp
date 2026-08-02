#include <gtk/gtk.h>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <stdlib.h>
#include <libgen.h>
#include <random>
#include <sstream>
#include <locale>
#include <ctime>
#include <cctype>

#include "openlipc/openlipc.h"

#include "music_backend.h"
#include "icons.h"

// KINAMP_VERSION comes from the project() version in CMakeLists.txt. Guessing a
// fallback here would make the update check offer bogus updates, so require it.
#ifndef KINAMP_VERSION
#error "KINAMP_VERSION is not defined - build with CMake."
#endif

#define KINAMP_GITHUB_URL "https://www.github.com/kbarni/KinAMP"
#define KINAMP_RELEASE_API "https://api.github.com/repos/kbarni/KinAMP/releases/latest"

enum PlaybackStrategy {
    NORMAL,
    REPEAT,
    RANDOM
};

enum SleepMode {
    SLEEP_OFF,
    SLEEP_TIMER,
    SLEEP_END_OF_PLAYLIST
};

struct AppData {
    MusicBackend *backend;
    GtkListStore *playlist_store;
    GtkListStore *radio_store; 
    GtkTreeView *playlist_treeview;
    GtkLabel *song_title_label;
    GtkLabel *time_label;

    bool is_hires;
    bool is_radio_mode;

    PlaybackStrategy current_strategy;
    int flIntensity;
    bool next_song_pending;
    bool dispUpdate;

    SleepMode sleep_mode;
    int sleep_minutes;      // duration last chosen in the sleep dialog
    time_t sleep_deadline;  // absolute time the timer fires (SLEEP_TIMER only)
    bool sleep_quit_pending;

    std::string next_song_path;
    std::string last_title;
    int current_index;
    GtkWidget *shuffle_button;
    GtkWidget *repeat_button;
    GtkWidget *volume_slider;
    
    GtkWidget *music_action_hbox;
    GtkWidget *radio_action_hbox;
    GtkWidget *switch_mode_button;
    
    GtkWidget *window;
};

// Defined below save_state(), but needed by the sleep timer in update_progress_cb().
void quit_app(AppData *app_data);

static LIPC * lipcInstance = 0;

void openLipcInstance() {
	if (lipcInstance == 0) {
		lipcInstance = LipcOpen("com.kbarni.kinamp");
	}
}

void closeLipcInstance() {
	if (lipcInstance != 0) {
		LipcClose(lipcInstance);
	}
}

void enableSleep() {
    LipcSetIntProperty(lipcInstance,"com.lab126.powerd","preventScreenSaver",0);
}

void disableSleep() {
    LipcSetIntProperty(lipcInstance,"com.lab126.powerd","preventScreenSaver",1);
}

void toggleFrontLight(AppData *ad){
    int intensity = 0;
    LipcGetIntProperty(lipcInstance,"com.lab126.powerd","flIntensity",&intensity);
    if(intensity == 0) {
        LipcSetIntProperty(lipcInstance,"com.lab126.powerd","flIntensity",ad->flIntensity);
    } else {
        ad->flIntensity=intensity;
        LipcSetIntProperty(lipcInstance,"com.lab126.powerd","flIntensity",0);
    }
}

void showLipcDialog(const char *dialogtitle, const char *dialogtext)
{
    char json[512];
    sprintf(json,"{ \"clientParams\":{ \"alertId\":\"appAlert1\", \"show\":true, \"customStrings\":[ { \"matchStr\":\"alertTitle\", \"replaceStr\":\"%s\" }, { \"matchStr\":\"alertText\", \"replaceStr\":\"%s\" } ] } }",dialogtitle,dialogtext);
    LipcSetStringProperty(lipcInstance,"com.lab126.pillow","pillowAlert",json);
}

GtkWidget* create_button_from_icon(const guint8* icon_data, int padding) {
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_inline(-1, icon_data, FALSE, NULL);
    GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
    
    GtkWidget *button = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(button), image);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_misc_set_padding(GTK_MISC(image), padding, padding);

    g_object_unref(pixbuf);
    return button;
}

void set_button_icon(GtkWidget *button, const unsigned char *icon_data) {
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_inline(-1, icon_data, FALSE, NULL);
    GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    gtk_button_set_image(GTK_BUTTON(button), image);
    gtk_widget_show(image);
}

struct ErrorPayload {
    char* msg;
    AppData* app_data;
};

gboolean show_error_dialog(gpointer data) {
    ErrorPayload* payload = (ErrorPayload*)data;
    showLipcDialog("KinAMP Error", payload->msg);  
    g_free(payload->msg);
    delete payload;
    return FALSE; // Remove from idle sources
}

void on_error_cb(const char* msg, void* user_data) {
    AppData *app_data = (AppData*)user_data;
    ErrorPayload* payload = new ErrorPayload();
    payload->msg = g_strdup(msg);
    payload->app_data = app_data;
    
    g_idle_add(show_error_dialog, payload);
}

void on_eos_cb(void* user_data) {
    AppData *app_data = (AppData*)user_data;

    // EOS not relevant for Radio usually, or maybe handle reconnection?
    if (app_data->is_radio_mode) {
        g_print("UI: End-of-Stream in Radio mode. Stopping.\n");
        return; 
    }

    g_print("UI: End-of-Stream reached. Planning next song.\n");

    // Shuffle and repeat never reach a real end of playlist, so there the sleep
    // request means "stop once the song that was playing is over".
    if (app_data->sleep_mode == SLEEP_END_OF_PLAYLIST && app_data->current_strategy != NORMAL) {
        g_print("UI: Sleep at end of playlist - stopping after the current song.\n");
        app_data->sleep_quit_pending = true;
        return;
    }

    GtkTreeModel *model = GTK_TREE_MODEL(app_data->playlist_store);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(app_data->playlist_treeview);
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        if (app_data->sleep_mode == SLEEP_END_OF_PLAYLIST) {
            app_data->sleep_quit_pending = true;
        }
        return;
    }

    GtkTreePath *current_path = gtk_tree_model_get_path(model, &iter);
    bool play_next = false;

    switch (app_data->current_strategy) {
        case NORMAL:
            gtk_tree_path_next(current_path);
            if (gtk_tree_model_get_iter(model, &iter, current_path)) {
                play_next = true;
            }
            break;
        case REPEAT:
            gtk_tree_path_next(current_path);
            if (!gtk_tree_model_get_iter(model, &iter, current_path)) {
                gtk_tree_path_free(current_path);
                current_path = gtk_tree_path_new_from_indices(0, -1);
                if (gtk_tree_model_get_iter(model, &iter, current_path)) {
                     play_next = true;
                }
            } else {
                play_next = true;
            }
            break;
        case RANDOM: {
            int count = gtk_tree_model_iter_n_children(model, NULL);
            if (count > 0) {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> distrib(0, count - 1);
                int random_index = distrib(gen);
                
                gtk_tree_path_free(current_path);
                current_path = gtk_tree_path_new_from_indices(random_index, -1);
                if (gtk_tree_model_get_iter(model, &iter, current_path)) {
                    play_next = true;
                }
            }
            break;
        }
    }

    if (play_next) {
        gchar *file_path = NULL;
        gtk_tree_model_get(model, &iter, 0, &file_path, -1);
        if (file_path) {
            app_data->next_song_path = file_path;
            app_data->next_song_pending = true;
            g_free(file_path);
        }
    } else if (app_data->sleep_mode == SLEEP_END_OF_PLAYLIST) {
        g_print("UI: Sleep at end of playlist - last song finished.\n");
        app_data->sleep_quit_pending = true;
    }

    gtk_tree_path_free(current_path);
}


gboolean update_progress_cb(gpointer data) {
    AppData *app_data = (AppData*)data;

    if (app_data->sleep_mode == SLEEP_TIMER && time(NULL) >= app_data->sleep_deadline) {
        g_print("UI: Sleep timer elapsed. Closing KinAMP.\n");
        app_data->sleep_quit_pending = true;
    }

    if (app_data->sleep_quit_pending) {
        app_data->sleep_quit_pending = false;
        app_data->sleep_mode = SLEEP_OFF;
        quit_app(app_data);
        return FALSE;
    }

    if (app_data->next_song_pending && !app_data->backend->is_playing && !app_data->backend->is_shutting_down()) {
        app_data->next_song_pending = false;
        
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app_data->playlist_store), &iter);
        while (valid) {
            gchar *path = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(app_data->playlist_store), &iter, 0, &path, -1);
            if (path && app_data->next_song_path == path) {
                GtkTreePath* tree_path = gtk_tree_model_get_path(GTK_TREE_MODEL(app_data->playlist_store), &iter);
                gtk_tree_view_set_cursor(app_data->playlist_treeview, tree_path, NULL, FALSE);
                gtk_tree_path_free(tree_path);
                g_free(path);
                break;
            }
            g_free(path);
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app_data->playlist_store), &iter);
        }
        app_data->backend->play_file(app_data->next_song_path.c_str());
        return TRUE;
    }


    if (app_data->backend->is_playing || app_data->backend->is_paused) {
        gint64 position = app_data->backend->get_position();

        char time_str[32];
        int pos_seconds = position / GST_SECOND;

        if (app_data->sleep_mode != SLEEP_OFF) {
            // While a sleep is armed the time display shows the countdown instead.
            if (app_data->sleep_mode == SLEEP_TIMER) {
                long remaining = (long)(app_data->sleep_deadline - time(NULL));
                if (remaining < 0) remaining = 0;
                snprintf(time_str, sizeof(time_str), (app_data->dispUpdate?"☾%02ld:%02ld":"  ☾  "), remaining / 60, remaining % 60);
            } else {
                snprintf(time_str, sizeof(time_str), "☾ end");
            }
        } else if (app_data->is_radio_mode) {
             snprintf(time_str, sizeof(time_str), (app_data->dispUpdate?" ● LIVE ":"   ●   "));
        } else {
            if (app_data->backend->is_paused) {
                snprintf(time_str, sizeof(time_str), (app_data->dispUpdate?"◫%02d:%02d":"  ◫  "), pos_seconds / 60, pos_seconds % 60);
            } else {
                snprintf(time_str, sizeof(time_str), (app_data->dispUpdate?"▷%02d:%02d":"  ▷  "), pos_seconds / 60, pos_seconds % 60);
            }
        }
        gtk_label_set_text(app_data->time_label, time_str);

        if (!app_data->is_radio_mode) {
            const char* full_path = app_data->backend->get_current_filepath();
            if (full_path && strlen(full_path) > 0) {
                char* path_copy = g_strdup(full_path);
                char* base = basename(path_copy);
                
                if (app_data->last_title != base) {
                    gtk_label_set_text(app_data->song_title_label, base);
                    app_data->last_title = base;
                }
                g_free(path_copy);
            }
        }

    } else {
        if (app_data->sleep_mode == SLEEP_TIMER) {
            char time_str[32];
            long remaining = (long)(app_data->sleep_deadline - time(NULL));
            if (remaining < 0) remaining = 0;
            snprintf(time_str, sizeof(time_str), (app_data->dispUpdate?"☾%02ld:%02ld":"  ☾  "), remaining / 60, remaining % 60);
            gtk_label_set_text(app_data->time_label, time_str);
        } else if (app_data->sleep_mode == SLEEP_END_OF_PLAYLIST) {
            gtk_label_set_text(app_data->time_label, "☾ end");
        } else {
            gtk_label_set_text(app_data->time_label, "▢--:--");
        }
        if (app_data->last_title != "No song playing") {
            gtk_label_set_text(app_data->song_title_label, "No song playing");
            app_data->last_title = "No song playing";
        }
    }

    return TRUE;
}

std::string get_config_path(const char* filename) {
    return std::string(filename);
}

void save_state(AppData *app_data) {
    // Only save music playlist if we are in music mode (or we should preserve it regardless)
    // Actually, playlist is always in playlist_store, even if hidden.
    std::string playlist_path = get_config_path(".kinamp_playlist.m3u");
    std::ofstream outfile(playlist_path.c_str());
    if (outfile.is_open()) {
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app_data->playlist_store), &iter);
        while (valid) {
            gchar *path = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(app_data->playlist_store), &iter, 0, &path, -1);
            if (path) {
                outfile << path << std::endl;
                g_free(path);
            }
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app_data->playlist_store), &iter);
        }
        outfile.close();
    }

    int current_index = -1;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(app_data->playlist_treeview);
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        if (path) {
            int *indices = gtk_tree_path_get_indices(path);
            if (indices) {
                current_index = indices[0];
            }
            gtk_tree_path_free(path);
        }
    }

    std::string config_path = get_config_path(".kinamp.conf");
    std::ofstream conffile(config_path.c_str());
    if (conffile.is_open()) {
        conffile.imbue(std::locale::classic());
        conffile << "current_index=" << current_index << std::endl;
        conffile << "playback_strategy=" << app_data->current_strategy << std::endl;
        conffile << "is_radio_mode=" << (app_data->is_radio_mode ? 1 : 0) << std::endl;
        conffile << "volume=" << app_data->backend->get_volume() << std::endl;
        conffile.close();
    }
}

// Closes KinAMP the normal way: restores the device power/BT settings, saves the
// state and leaves gtk_main() so the device is free to suspend again.
void quit_app(AppData *app_data) {
    save_state(app_data);
    app_data->backend->stop();
    LipcSetIntProperty(lipcInstance,"com.lab126.powerd","flIntensity",app_data->flIntensity);
    LipcSetIntProperty(lipcInstance,"com.lab126.btfd","ensureBTconnection",0);
    enableSleep();
    closeLipcInstance();
    gtk_main_quit();
    exit(0);
}

// If the line is "key=value", returns the value, otherwise NULL.
static const char* config_value(const std::string &line, const char *key) {
    size_t key_len = strlen(key);
    if (line.compare(0, key_len, key) != 0) return NULL;
    return line.c_str() + key_len;
}

// Reads a number the way save_state() wrote it. atof() would follow the current
// locale and return 0 for "0.75" wherever the decimal separator is a comma.
static double parse_double(const char *text, double fallback) {
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    double value;
    if (!(stream >> value)) return fallback;
    return value;
}

void load_state(AppData *app_data) {
    std::string playlist_path = get_config_path(".kinamp_playlist.m3u");
    std::ifstream infile(playlist_path.c_str());
    if (infile.is_open()) {
        gtk_list_store_clear(app_data->playlist_store);
        std::string line;
        while (std::getline(infile, line)) {
            if (!line.empty()) {
                GtkTreeIter iter;
                gtk_list_store_append(app_data->playlist_store, &iter);
                gtk_list_store_set(app_data->playlist_store, &iter, 0, line.c_str(), -1);
            }
        }
        infile.close();
    }

    std::string config_path = get_config_path(".kinamp.conf");
    std::ifstream conffile(config_path.c_str());
    int current_index = -1;
    if (conffile.is_open()) {
        std::string line;
        while (std::getline(conffile, line)) {
            if (const char *value = config_value(line, "current_index=")) {
                current_index = atoi(value);
            }
            if (const char *value = config_value(line, "playback_strategy=")) {
                int strategy = atoi(value);
                if (strategy < NORMAL || strategy > RANDOM) strategy = NORMAL;
                app_data->current_strategy = (PlaybackStrategy)strategy;
                if (app_data->current_strategy == RANDOM) {
                    set_button_icon(app_data->shuffle_button, app_data->is_hires ? shuffle_on_icon : shuffle_on_icon_lr);
                    set_button_icon(app_data->repeat_button, app_data->is_hires ? repeat_icon : repeat_icon_lr);
                } else if (app_data->current_strategy == REPEAT) {
                    set_button_icon(app_data->shuffle_button, app_data->is_hires ? shuffle_icon : shuffle_icon_lr);
                    set_button_icon(app_data->repeat_button, app_data->is_hires ? repeat_on_icon : repeat_on_icon_lr);
                } else {
                    set_button_icon(app_data->shuffle_button, app_data->is_hires ? shuffle_icon : shuffle_icon_lr);
                    set_button_icon(app_data->repeat_button, app_data->is_hires ? repeat_icon : repeat_icon_lr);
                }
            }
            if (const char *value = config_value(line, "is_radio_mode=")) {
                app_data->is_radio_mode = (atoi(value) != 0);
            }
            if (const char *value = config_value(line, "volume=")) {
                double volume = parse_double(value, 1.0);
                if (volume < 0.0) volume = 0.0;
                if (volume > 1.0) volume = 1.0;
                app_data->backend->set_volume(volume);
                gtk_range_set_value(GTK_RANGE(app_data->volume_slider), volume);
            }
        }
        conffile.close();
    }
    
    if (app_data->is_radio_mode) {
        set_button_icon(app_data->switch_mode_button, app_data->is_hires ? musiclibrary_icon : musiclibrary_icon_lr);
        gtk_widget_hide(app_data->music_action_hbox);
        gtk_widget_show(app_data->radio_action_hbox);
        gtk_tree_view_set_model(app_data->playlist_treeview, GTK_TREE_MODEL(app_data->radio_store));
    }
    if (current_index != -1) {
        GtkTreePath *path = gtk_tree_path_new_from_indices(current_index, -1);
        if (path) {
            gtk_tree_view_set_cursor(app_data->playlist_treeview, path, NULL, FALSE);
            gtk_tree_path_free(path);
        }
    }
}

void load_radio_stations(AppData *app_data) {
    std::string path = get_config_path(".kinamp_radio.txt");
    std::ifstream infile(path.c_str());
    gtk_list_store_clear(app_data->radio_store);
    if (infile.is_open()) {
        std::string line;
        while (std::getline(infile, line)) {
             size_t pos = line.find('|');
             if (pos != std::string::npos) {
                 std::string name = line.substr(0, pos);
                 std::string url = line.substr(pos + 1);
                 GtkTreeIter iter;
                 gtk_list_store_append(app_data->radio_store, &iter);
                 gtk_list_store_set(app_data->radio_store, &iter, 0, name.c_str(), 1, url.c_str(), -1);
             }
        }
        infile.close();
    }
}

void save_radio_stations(AppData *app_data) {
    std::string path = get_config_path(".kinamp_radio.txt");
    std::ofstream outfile(path.c_str());
    if (outfile.is_open()) {
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app_data->radio_store), &iter);
        while (valid) {
            gchar *name, *url;
            gtk_tree_model_get(GTK_TREE_MODEL(app_data->radio_store), &iter, 0, &name, 1, &url, -1);
            if (name && url) {
                outfile << name << "|" << url << std::endl;
            }
            g_free(name);
            g_free(url);
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app_data->radio_store), &iter);
        }
        outfile.close();
    }
}


void set_label_font(GtkWidget *label, const char *font_desc_str) {
    PangoFontDescription *font_desc = pango_font_description_from_string(font_desc_str);
    gtk_widget_modify_font(label, font_desc);
    pango_font_description_free(font_desc);
}

void add_directory_to_playlist(const char *dir_path, GtkListStore *playlist_store) {
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return;
    }

    std::vector<std::string> files;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                std::string new_path = std::string(dir_path) + "/" + entry->d_name;
                add_directory_to_playlist(new_path.c_str(), playlist_store);
            }
        }
        else {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcmp(ext, ".mp3") == 0 || strcmp(ext, ".flac") == 0 || strcmp(ext, ".wav") == 0)) {
                files.push_back(std::string(dir_path) + "/" + entry->d_name);
            }
        }
    }
    closedir(dir);

    std::sort(files.begin(), files.end());

    for (const auto& file_path : files) {
        GtkTreeIter iter;
        gtk_list_store_append(playlist_store, &iter);
        gtk_list_store_set(playlist_store, &iter, 0, file_path.c_str(), -1);
    }
}

void play_selected_song(AppData* app_data) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(app_data->playlist_treeview);
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        if (app_data->is_radio_mode) {
             gchar *name = NULL;
             gchar *url = NULL;
             gtk_tree_model_get(model, &iter, 0, &name, 1, &url, -1);
             if (url) {
                 app_data->backend->play_file(url);
                 if (name) {
                     gtk_label_set_text(app_data->song_title_label, name);
                     app_data->last_title = name;
                 }
                 g_free(name);
                 g_free(url);
             }
        } else {
            gchar *file_path = NULL;
            gtk_tree_model_get(model, &iter, 0, &file_path, -1);
            if (file_path) {
                app_data->backend->play_file(file_path);
                char* path_copy = g_strdup(file_path);
                char* base = basename(path_copy);
                gtk_label_set_text(app_data->song_title_label, base);

                app_data->last_title = base;
                
                g_free(path_copy);
                g_free(file_path);
            }
        }
    }
}

void on_previous_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(app_data->playlist_treeview);
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        if (gtk_tree_path_prev(path)) {
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(app_data->playlist_treeview), path, NULL, FALSE);
            play_selected_song(app_data);
        }
        gtk_tree_path_free(path);
    }
}

void on_play_pause_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;

    if (app_data->backend->is_shutting_down()) {
        g_print("UI: Backend is stopping, ignoring play/pause click.\n");
        return;
    }

    if (app_data->backend->is_playing || app_data->backend->is_paused) {
        app_data->backend->pause();
        return;
    }
    
    play_selected_song(app_data);
}

void on_stop_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    app_data->backend->stop();
}

void on_next_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(app_data->playlist_treeview);
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        gtk_tree_path_next(path);
        
        if (gtk_tree_model_get_iter(model, &iter, path)) {
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(app_data->playlist_treeview), path, NULL, FALSE);
            play_selected_song(app_data);
        }
        gtk_tree_path_free(path);
    }
}

void on_background_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    LipcSetIntProperty(lipcInstance,"com.lab126.powerd","flIntensity",app_data->flIntensity);
    enableSleep();
    closeLipcInstance();
    save_state(app_data);
    app_data->backend->stop();
    gtk_main_quit();
    if(app_data->is_radio_mode) {
        exit(11);
    } else {
        exit(10);
    }
    
}

void on_close_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    quit_app((AppData*)data);
}

void on_shuffle_clicked(GtkWidget *widget, gpointer data) {
    AppData *app_data = (AppData*)data;
    if (app_data->current_strategy == RANDOM) {
        app_data->current_strategy = NORMAL;
        set_button_icon(widget, app_data->is_hires ? shuffle_icon : shuffle_icon_lr);
    } else {
        app_data->current_strategy = RANDOM;
        set_button_icon(widget, app_data->is_hires ? shuffle_on_icon : shuffle_on_icon_lr);
        set_button_icon(app_data->repeat_button, app_data->is_hires ? repeat_icon : repeat_icon_lr);
    }
    g_print("Shuffle mode toggled. New strategy: %d\n", app_data->current_strategy);
}

void on_repeat_clicked(GtkWidget *widget, gpointer data) {
    AppData *app_data = (AppData*)data;
    if (app_data->current_strategy == REPEAT) {
        app_data->current_strategy = NORMAL;
        set_button_icon(widget, app_data->is_hires ? repeat_icon : repeat_icon_lr);
    } else {
        app_data->current_strategy = REPEAT;
        set_button_icon(widget, app_data->is_hires ? repeat_on_icon : repeat_on_icon_lr);
        set_button_icon(app_data->shuffle_button, app_data->is_hires ? shuffle_icon : shuffle_icon_lr);
    }
    g_print("Repeat mode toggled. New strategy: %d\n", app_data->current_strategy);
}

void on_fl_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    toggleFrontLight(app_data);
}

void on_bluetooth_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    LipcSetStringProperty(lipcInstance,"com.lab126.btfd","BTenable","1:1");
    LipcSetStringProperty(lipcInstance,"com.lab126.pillow","customDialog","{\"name\":\"bt_wizard_dialog\", \"clientParams\": {\"show\":true, \"winmgrModal\":true, \"replySrc\":\"\"}}");
}

void on_displayUpdate_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    app_data->dispUpdate = !(app_data->dispUpdate);
}

void on_volume_changed(GtkRange *range, gpointer data) {
    AppData *app_data = (AppData*)data;
    double volume = gtk_range_get_value(range);
    app_data->backend->set_volume(volume);
}

void on_add_file_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    GtkListStore *playlist_store = app_data->playlist_store;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("L:A_N:application_PC:TS_ID:com.kbarni.kinamp",
                                                  NULL,
                                                  GTK_FILE_CHOOSER_ACTION_OPEN,
                                                  GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                  GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                                                  NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Music files");
    gtk_file_filter_add_pattern(filter, "*.mp3");
    gtk_file_filter_add_pattern(filter, "*.flac");
    gtk_file_filter_add_pattern(filter, "*.wav");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
        for (GSList *l = filenames; l != NULL; l = l->next) {
            char *file_path = (char*)l->data;
            GtkTreeIter iter;
            gtk_list_store_append(playlist_store, &iter);
            gtk_list_store_set(playlist_store, &iter, 0, file_path, -1);
            g_free(file_path);
        }
        g_slist_free(filenames);
    }

    gtk_widget_destroy(dialog);
}
void on_add_folder_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    GtkListStore *playlist_store = app_data->playlist_store;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("L:A_N:application_PC:TS_ID:com.kbarni.kinamp",
                                                  NULL,
                                                  GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                  GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                  GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                                                  NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *folder_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        add_directory_to_playlist(folder_path, playlist_store);
        g_free(folder_path);
    }

    gtk_widget_destroy(dialog);
}
void on_clear_playlist_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    GtkListStore *playlist_store = app_data->playlist_store;
    gtk_list_store_clear(playlist_store);
}
void on_save_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    GtkListStore *playlist_store = app_data->playlist_store;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("L:A_N:application_PC:TS_ID:com.kbarni.kinamp",
                                                  NULL,
                                                  GTK_FILE_CHOOSER_ACTION_SAVE,
                                                  GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                  GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
                                                  NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "playlist.m3u");

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "M3U playlist");
    gtk_file_filter_add_pattern(filter, "*.m3u");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        std::ofstream outfile(filename);
        if (outfile.is_open()) {
            GtkTreeIter iter;
            gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(playlist_store), &iter);
            while (valid) {
                gchar *path = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(playlist_store), &iter, 0, &path, -1);
                if (path) {
                    outfile << path << std::endl;
                    g_free(path);
                }
                valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(playlist_store), &iter);
            }
            outfile.close();
        }
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}
void on_load_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    GtkListStore *playlist_store = app_data->playlist_store;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("L:A_N:application_PC:TS_ID:com.kbarni.kinamp",
                                                  NULL,
                                                  GTK_FILE_CHOOSER_ACTION_OPEN,
                                                  GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                  GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                                                  NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "M3U playlist");
    gtk_file_filter_add_pattern(filter, "*.m3u");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        std::ifstream infile(filename);
        if (infile.is_open()) {
            gtk_list_store_clear(playlist_store);
            std::string line;
            while (std::getline(infile, line)) {
                if (!line.empty()) {
                    GtkTreeIter iter;
                    gtk_list_store_append(playlist_store, &iter);
                    gtk_list_store_set(playlist_store, &iter, 0, line.c_str(), -1);
                }
            }
            infile.close();
        }
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

void on_add_station_clicked(GtkWidget *widget, gpointer data) {
    AppData *app_data = (AppData*)data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("L:A_N:Add Radio Station_PC:TS_ID:add_station",
                                                    GTK_WINDOW(gtk_widget_get_toplevel(widget)),
                                                    GTK_DIALOG_MODAL,
                                                    GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
                                                    GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                    NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_table_new(2, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_container_add(GTK_CONTAINER(content_area), grid);

    GtkWidget *name_label = gtk_label_new("Name:");
    GtkWidget *url_label = gtk_label_new("URL:");
    GtkWidget *name_entry = gtk_entry_new();
    GtkWidget *url_entry = gtk_entry_new();

    gtk_table_attach_defaults(GTK_TABLE(grid), name_label, 0, 1, 0, 1);
    gtk_table_attach_defaults(GTK_TABLE(grid), name_entry, 1, 2, 0, 1);
    gtk_table_attach_defaults(GTK_TABLE(grid), url_label, 0, 1, 1, 2);
    gtk_table_attach_defaults(GTK_TABLE(grid), url_entry, 1, 2, 1, 2);

    gtk_widget_show_all(grid);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(name_entry));
        const char *url = gtk_entry_get_text(GTK_ENTRY(url_entry));
        if (name && url && strlen(name) > 0 && strlen(url) > 0) {
            GtkTreeIter iter;
            gtk_list_store_append(app_data->radio_store, &iter);
            gtk_list_store_set(app_data->radio_store, &iter, 0, name, 1, url, -1);
            save_radio_stations(app_data);
        }
    }
    gtk_widget_destroy(dialog);
}

void on_remove_station_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(app_data->playlist_treeview);
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gtk_list_store_remove(app_data->radio_store, &iter);
        save_radio_stations(app_data);
    }
}

void on_switch_mode_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    app_data->backend->stop(); 

    if (app_data->is_radio_mode) {
        // Switch to Music
        app_data->is_radio_mode = false;
        set_button_icon(app_data->switch_mode_button, app_data->is_hires ? radio_icon : radio_icon_lr);
        gtk_widget_show(app_data->music_action_hbox);
        gtk_widget_hide(app_data->radio_action_hbox);
        gtk_tree_view_set_model(app_data->playlist_treeview, GTK_TREE_MODEL(app_data->playlist_store));
        
        save_radio_stations(app_data); 
        // We could restore selection here if we saved it in AppData
    } else {
        // Switch to Radio
        save_state(app_data); 
        app_data->is_radio_mode = true;
        set_button_icon(app_data->switch_mode_button, app_data->is_hires ? musiclibrary_icon : musiclibrary_icon_lr);
        gtk_widget_hide(app_data->music_action_hbox);
        gtk_widget_show(app_data->radio_action_hbox);
        gtk_tree_view_set_model(app_data->playlist_treeview, GTK_TREE_MODEL(app_data->radio_store));
    }
}


// ---------------------------------------------------------------- Sleep mode

struct SleepDialogData {
    AppData *app_data;
    GtkWidget *radio_timer;
    GtkWidget *minutes_label;
    int minutes;
};

static void sleep_refresh_minutes(SleepDialogData *sd) {
    char markup[64];
    snprintf(markup, sizeof(markup), "<b>%d min</b>", sd->minutes);
    gtk_label_set_markup(GTK_LABEL(sd->minutes_label), markup);
}

static void on_sleep_delta_clicked(GtkWidget *widget, gpointer data) {
    SleepDialogData *sd = (SleepDialogData*)data;
    int delta = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "delta"));

    sd->minutes += delta;
    if (sd->minutes < 1) sd->minutes = 1;
    if (sd->minutes > 300) sd->minutes = 300;
    sleep_refresh_minutes(sd);

    // Touching the duration implies the user wants the timer.
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd->radio_timer), TRUE);
}

static GtkWidget* create_sleep_delta_button(SleepDialogData *sd, const char *label, int delta) {
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_container_set_border_width(GTK_CONTAINER(button), 2);
    g_object_set_data(G_OBJECT(button), "delta", GINT_TO_POINTER(delta));
    g_signal_connect(button, "clicked", G_CALLBACK(on_sleep_delta_clicked), sd);
    return button;
}

void on_sleep_clicked(GtkWidget *widget, gpointer data) {
    AppData *app_data = (AppData*)data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("L:D_N:dialog_ID:com.kbarni.kinamp",
                                                    GTK_WINDOW(gtk_widget_get_toplevel(widget)),
                                                    GTK_DIALOG_MODAL,
                                                    GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
                                                    NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(content_area), vbox);

    GtkWidget *heading = gtk_label_new("<b>Sleep mode</b>");
    gtk_label_set_use_markup(GTK_LABEL(heading), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), heading, FALSE, FALSE, 5);

    SleepDialogData sd;
    sd.app_data = app_data;
    sd.minutes = app_data->sleep_minutes;

    GtkWidget *radio_off = gtk_radio_button_new_with_label(NULL, "Deactivate");
    GSList *group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(radio_off));
    GtkWidget *radio_timer = gtk_radio_button_new_with_label(group, "Stop after");
    group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(radio_timer));
    GtkWidget *radio_eop = gtk_radio_button_new_with_label(group, "Stop at the end of the playlist");
    sd.radio_timer = radio_timer;

    gtk_box_pack_start(GTK_BOX(vbox), radio_off, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), radio_timer, FALSE, FALSE, 0);

    sd.minutes_label = gtk_label_new(NULL);
    sleep_refresh_minutes(&sd);
    GtkWidget *minutes_frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(minutes_frame), GTK_SHADOW_IN);
    GtkWidget *minutes_padding = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_alignment_set_padding(GTK_ALIGNMENT(minutes_padding), 4, 4, 10, 10);
    gtk_container_add(GTK_CONTAINER(minutes_padding), sd.minutes_label);
    gtk_container_add(GTK_CONTAINER(minutes_frame), minutes_padding);

    GtkWidget *delta_hbox = gtk_hbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(delta_hbox), create_sleep_delta_button(&sd, "-10", -10), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(delta_hbox), create_sleep_delta_button(&sd, "-1", -1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(delta_hbox), minutes_frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(delta_hbox), create_sleep_delta_button(&sd, "+1", 1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(delta_hbox), create_sleep_delta_button(&sd, "+10", 10), FALSE, FALSE, 0);

    // Indent the duration row so it reads as belonging to "Stop after".
    GtkWidget *delta_indent = gtk_alignment_new(0, 0.5, 0, 0);
    gtk_alignment_set_padding(GTK_ALIGNMENT(delta_indent), 0, 5, 25, 0);
    gtk_container_add(GTK_CONTAINER(delta_indent), delta_hbox);
    gtk_box_pack_start(GTK_BOX(vbox), delta_indent, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), radio_eop, FALSE, FALSE, 0);
    // A radio stream has no end of playlist to wait for.
    gtk_widget_set_sensitive(radio_eop, !app_data->is_radio_mode);

    switch (app_data->sleep_mode) {
        case SLEEP_TIMER:
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio_timer), TRUE);
            break;
        case SLEEP_END_OF_PLAYLIST:
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio_eop), TRUE);
            break;
        default:
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio_off), TRUE);
            break;
    }

    gtk_widget_show_all(vbox);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        app_data->sleep_minutes = sd.minutes;
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio_timer))) {
            app_data->sleep_mode = SLEEP_TIMER;
            app_data->sleep_deadline = time(NULL) + (time_t)sd.minutes * 60;
            g_print("Sleep: closing KinAMP in %d minutes.\n", sd.minutes);
        } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio_eop))) {
            app_data->sleep_mode = SLEEP_END_OF_PLAYLIST;
            g_print("Sleep: closing KinAMP at the end of the playlist.\n");
        } else {
            app_data->sleep_mode = SLEEP_OFF;
            g_print("Sleep: deactivated.\n");
        }
    }

    gtk_widget_destroy(dialog);
}

// --------------------------------------------------------- About / update check

// Runs a command and returns everything it wrote to stdout.
static std::string run_command(const std::string &cmd) {
    std::string output;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return output;

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
    pclose(pipe);
    return output;
}

// Minimal extraction of a "key": "value" pair - enough for the release metadata.
static std::string json_string_value(const std::string &json, const std::string &key, size_t from = 0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, from);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    size_t start = json.find('"', pos);
    if (start == std::string::npos) return "";
    size_t end = json.find('"', start + 1);
    if (end == std::string::npos) return "";
    return json.substr(start + 1, end - start - 1);
}

// "2.2" -> {2,2}. Stops at the first part that isn't a number, so "2.1-beta" -> {2,1}.
static std::vector<int> parse_version(const std::string &version) {
    std::vector<int> parts;
    size_t i = 0;
    while (i < version.size() && !isdigit((unsigned char)version[i])) i++; // skip a leading "v"
    while (i < version.size()) {
        if (isdigit((unsigned char)version[i])) {
            int value = 0;
            while (i < version.size() && isdigit((unsigned char)version[i])) {
                value = value * 10 + (version[i++] - '0');
            }
            parts.push_back(value);
        } else if (version[i] == '.') {
            i++;
        } else {
            break;
        }
    }
    return parts;
}

// Returns true if "candidate" is a newer version than "current".
static bool version_is_newer(const std::string &candidate, const std::string &current) {
    std::vector<int> a = parse_version(candidate);
    std::vector<int> b = parse_version(current);
    if (a.empty()) return false;

    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
        int va = (i < a.size()) ? a[i] : 0;
        int vb = (i < b.size()) ? b[i] : 0;
        if (va != vb) return va > vb;
    }
    return false;
}

// Writes a helper script that downloads and unpacks the release once KinAMP is
// gone, then restarts the player. It cannot be done in-process: the update
// replaces both the running binary and the launcher script.
static bool launch_updater(const std::string &url, const std::string &new_version) {
    const char *script_path = "/mnt/us/kinamp_update.sh";

    gchar *quoted_url = g_shell_quote(url.c_str());
    gchar *quoted_version = g_shell_quote(new_version.c_str());

    std::ofstream script(script_path);
    if (!script.is_open()) {
        g_free(quoted_url);
        g_free(quoted_version);
        return false;
    }

    script <<
        "#!/bin/sh\n"
        "# Generated by KinAMP - installs an update and restarts the player.\n"
        "URL=" << quoted_url << "\n"
        "NEWVERSION=" << quoted_version << "\n"
        "ZIP=/mnt/us/kinamp_update.zip\n"
        "\n"
        "alert() {\n"
        "    TEXT=$(printf '%s' \"$1\" | sed 's/\"/\\\\\"/g')\n"
        "    JSON='{ \"clientParams\":{ \"alertId\":\"appAlert1\", \"show\":true, \"customStrings\":[ "
        "{ \"matchStr\":\"alertTitle\", \"replaceStr\":\"KinAMP\" }, "
        "{ \"matchStr\":\"alertText\", \"replaceStr\":\"'\"$TEXT\"'\" } ] } }'\n"
        "    lipc-set-prop com.lab126.pillow pillowAlert \"$JSON\"\n"
        "}\n"
        "\n"
        "# Wait for KinAMP and its launcher script to exit - both get overwritten.\n"
        "sleep 5\n"
        "\n"
        "wget -q -T 30 --no-check-certificate -O \"$ZIP\" \"$URL\"\n"
        "if [ ! -s \"$ZIP\" ]; then\n"
        "    rm -f \"$ZIP\"\n"
        "    alert \"Update download failed. Please check your Wi-Fi connection.\"\n"
        "    exit 1\n"
        "fi\n"
        "\n"
        "if unzip -o -q \"$ZIP\" -d /mnt/us; then\n"
        "    rm -f \"$ZIP\"\n"
        "    alert \"KinAMP was updated to version $NEWVERSION. Restarting...\"\n"
        "    cd /mnt/us/KinAMP && exec ./startkinamp.sh\n"
        "else\n"
        "    alert \"Could not unpack the update. Extract $ZIP to the root of the Kindle manually.\"\n"
        "    exit 1\n"
        "fi\n";
    script.close();

    g_free(quoted_url);
    g_free(quoted_version);

    std::string cmd = std::string("/bin/sh ") + script_path + " >/dev/null 2>&1 &";
    return system(cmd.c_str()) == 0;
}

struct AboutDialogData {
    AppData *app_data;
    GtkWidget *dialog;
    GtkWidget *status_label;
    std::string download_url;
    std::string new_version;
    bool install_requested;
};

static void about_set_status(AboutDialogData *ad, const char *text) {
    gtk_label_set_text(GTK_LABEL(ad->status_label), text);
    gtk_widget_show(ad->status_label);
    // Repaint before the blocking download below.
    while (gtk_events_pending()) gtk_main_iteration();
}

static void on_check_updates_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AboutDialogData *ad = (AboutDialogData*)data;

    about_set_status(ad, "Checking for updates...");

    std::string json = run_command("wget -q -T 15 --no-check-certificate -O - \"" KINAMP_RELEASE_API "\" 2>/dev/null");
    if (json.empty()) {
        about_set_status(ad, "Could not reach GitHub.\nPlease check your Wi-Fi connection.");
        return;
    }

    std::string tag = json_string_value(json, "tag_name");
    if (tag.empty()) {
        about_set_status(ad, "Could not read the release information.");
        return;
    }

    if (!version_is_newer(tag, KINAMP_VERSION)) {
        about_set_status(ad, "KinAMP " KINAMP_VERSION " is up to date.");
        return;
    }

    // Find the release archive among the assets.
    std::string url;
    size_t from = 0;
    while (true) {
        std::string candidate = json_string_value(json, "browser_download_url", from);
        if (candidate.empty()) break;
        if (candidate.size() > 4 && candidate.compare(candidate.size() - 4, 4, ".zip") == 0) {
            url = candidate;
            break;
        }
        from = json.find(candidate, from);
        from = (from == std::string::npos) ? json.size() : from + candidate.size();
    }

    if (url.empty()) {
        std::string msg = "Version " + tag + " is available.\nPlease download it from GitHub.";
        about_set_status(ad, msg.c_str());
        return;
    }

    std::string question = "KinAMP " + tag + " is available (you have " KINAMP_VERSION ").\n\n"
                           "KinAMP will close, install the update and restart.\n"
                           "Install it now?";
    GtkWidget *confirm = gtk_message_dialog_new(GTK_WINDOW(ad->dialog),
                                                GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_QUESTION,
                                                GTK_BUTTONS_YES_NO,
                                                "%s", question.c_str());
    gtk_window_set_title(GTK_WINDOW(confirm), "L:D_N:dialog_ID:com.kbarni.kinamp");
    gint answer = gtk_dialog_run(GTK_DIALOG(confirm));
    gtk_widget_destroy(confirm);

    if (answer == GTK_RESPONSE_YES) {
        ad->download_url = url;
        ad->new_version = tag;
        ad->install_requested = true;
        // Leave the About dialog; the update is started once it is destroyed.
        gtk_dialog_response(GTK_DIALOG(ad->dialog), GTK_RESPONSE_ACCEPT);
    } else {
        about_set_status(ad, "");
    }
}

void on_info_clicked(GtkWidget *widget, gpointer data) {
    AppData *app_data = (AppData*)data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("L:D_N:dialog_ID:com.kbarni.kinamp",
                                                    GTK_WINDOW(gtk_widget_get_toplevel(widget)),
                                                    GTK_DIALOG_MODAL,
                                                    GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
                                                    NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(content_area), vbox);

    GtkWidget *name_label = gtk_label_new("<big><b>KinAMP</b></big>");
    gtk_label_set_use_markup(GTK_LABEL(name_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), name_label, FALSE, FALSE, 5);

    GtkWidget *desc_label = gtk_label_new("Kindle media player\nVersion " KINAMP_VERSION "\n(c) 2026 kbarni");
    gtk_label_set_justify(GTK_LABEL(desc_label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), desc_label, FALSE, FALSE, 5);

    GtkWidget *separator = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(vbox), separator, FALSE, FALSE, 5);

    GtkWidget *link_label = gtk_label_new(KINAMP_GITHUB_URL);
    gtk_label_set_selectable(GTK_LABEL(link_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), link_label, FALSE, FALSE, 0);

    AboutDialogData ad;
    ad.app_data = app_data;
    ad.dialog = dialog;
    ad.install_requested = false;

    GtkWidget *update_button = gtk_button_new_with_label("Check for updates");
    gtk_container_set_border_width(GTK_CONTAINER(update_button), 5);
    GtkWidget *update_align = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(update_align), update_button);
    gtk_box_pack_start(GTK_BOX(vbox), update_align, FALSE, FALSE, 5);

    ad.status_label = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(ad.status_label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), ad.status_label, FALSE, FALSE, 0);

    g_signal_connect(update_button, "clicked", G_CALLBACK(on_check_updates_clicked), &ad);

    gtk_widget_show_all(vbox);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (ad.install_requested) {
        if (launch_updater(ad.download_url, ad.new_version)) {
            showLipcDialog("KinAMP", "Downloading the update. KinAMP will restart when it is installed.");
            quit_app(app_data);
        } else {
            showLipcDialog("KinAMP", "Could not start the update.");
        }
    }
}

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    MusicBackend backend;
    AppData app_data;

    GdkScreen *screen = gdk_screen_get_default();
    gint width = gdk_screen_get_width(screen);
    gint height = gdk_screen_get_height(screen);
    app_data.is_hires = (width >= 1000);
    g_print("Detected resolution: %dx%d, using %s mode\n", width, height, (app_data.is_hires?"High res":"Low res"));

    app_data.backend = &backend;
    app_data.current_strategy = NORMAL;
    app_data.next_song_pending = false;
    app_data.flIntensity = 0;
    app_data.dispUpdate=true;
    app_data.is_radio_mode = false;
    app_data.sleep_mode = SLEEP_OFF;
    app_data.sleep_minutes = 30;
    app_data.sleep_deadline = 0;
    app_data.sleep_quit_pending = false;

    backend.set_eos_callback(on_eos_cb, &app_data);
    backend.set_error_callback(on_error_cb, &app_data);

    openLipcInstance();
    disableSleep();
    LipcGetIntProperty(lipcInstance,"com.lab126.powerd","flIntensity",&app_data.flIntensity);

    LipcSetIntProperty(lipcInstance,"com.lab126.btfd","ensureBTconnection",1);
    LipcSetStringProperty(lipcInstance,"com.lab126.btfd","BTenable","1:1");

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    app_data.window = window;
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 400);
    gtk_window_set_title(GTK_WINDOW(window), "L:A_N:application_PC:T_ID:com.kbarni.kinamp");
    g_signal_connect(window, "destroy", G_CALLBACK(on_close_clicked), &app_data);

    GtkWidget *main_vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 20);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    GtkWidget *player_vbox = gtk_vbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(main_vbox), player_vbox, FALSE, FALSE, 0);

    GdkPixbuf *title_pixbuf = gdk_pixbuf_new_from_inline(-1, app_data.is_hires ? title_image : title_image_lr, FALSE, NULL);
    GtkWidget *title_image_widget = gtk_image_new_from_pixbuf(title_pixbuf);
    g_object_unref(title_pixbuf);

    GtkWidget *title_alignment = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(title_alignment), title_image_widget);
    gtk_box_pack_start(GTK_BOX(player_vbox), title_alignment, FALSE, FALSE, 0);

    GtkWidget *info_hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(player_vbox), info_hbox, FALSE, FALSE, 0);

    GtkWidget *time_label = gtk_label_new("▢--:--");
    app_data.time_label = GTK_LABEL(time_label);
    set_label_font(time_label, app_data.is_hires ? "Mono Bold 20" : "Mono Bold 10");
    GtkWidget *time_frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(time_frame), time_label);
    gtk_box_pack_start(GTK_BOX(info_hbox), time_frame, FALSE, FALSE, 0);

    GtkWidget *song_title_label = gtk_label_new("No song playing");
    app_data.song_title_label = GTK_LABEL(song_title_label);
    set_label_font(song_title_label, app_data.is_hires ? "Sans 14" : "Sans 10");
    gtk_box_pack_start(GTK_BOX(info_hbox), song_title_label, TRUE, TRUE, 0);

    GtkWidget *separator = gtk_hseparator_new();
    gtk_widget_set_size_request(separator, -1, app_data.is_hires ? 10 : 5);
    gtk_box_pack_start(GTK_BOX(player_vbox), separator, FALSE, FALSE, 5);

    GtkWidget *controls_hbox = gtk_hbox_new(FALSE, 2); 
    gtk_box_pack_start(GTK_BOX(player_vbox), controls_hbox, FALSE, FALSE, 0);

    int btn_padding = app_data.is_hires ? 5 : 2;

    GtkWidget *prev_button = create_button_from_icon(app_data.is_hires ? skip_previous_icon : skip_previous_icon_lr, btn_padding);
    GtkWidget *play_button = create_button_from_icon(app_data.is_hires ? play_pause_icon : play_pause_icon_lr, btn_padding);
    GtkWidget *stop_button = create_button_from_icon(app_data.is_hires ? stop_icon : stop_icon_lr, btn_padding);
    GtkWidget *next_button = create_button_from_icon(app_data.is_hires ? skip_next_icon : skip_next_icon_lr, btn_padding);
    
    GtkWidget *shuffle_button = create_button_from_icon(app_data.is_hires ? shuffle_icon : shuffle_icon_lr, btn_padding);
    app_data.shuffle_button = shuffle_button;
    GtkWidget *repeat_button = create_button_from_icon(app_data.is_hires ? repeat_icon : repeat_icon_lr, btn_padding);
    app_data.repeat_button = repeat_button;

    GtkWidget *sleep_button = create_button_from_icon(app_data.is_hires ? sleep_icon : sleep_icon_lr, btn_padding);
    GtkWidget *dispupdate_button = create_button_from_icon(app_data.is_hires ? display_icon : display_icon_lr, btn_padding);
    GtkWidget *frontlight_button = create_button_from_icon(app_data.is_hires ? sunny_icon : sunny_icon_lr, btn_padding);
    GtkWidget *bluetooth_button = create_button_from_icon(app_data.is_hires ? bluetooth_icon : bluetooth_icon_lr, btn_padding);
    GtkWidget *info_button = create_button_from_icon(app_data.is_hires ? info_icon : info_icon_lr, btn_padding);
    GtkWidget *background_button = create_button_from_icon(app_data.is_hires ? standby_icon : standby_icon_lr, btn_padding);
    GtkWidget *close_button = create_button_from_icon(app_data.is_hires ? close_icon : close_icon_lr, btn_padding);

    g_signal_connect(prev_button, "clicked", G_CALLBACK(on_previous_clicked), &app_data);
    g_signal_connect(play_button, "clicked", G_CALLBACK(on_play_pause_clicked), &app_data);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_stop_clicked), &app_data);
    g_signal_connect(next_button, "clicked", G_CALLBACK(on_next_clicked), &app_data);
    
    g_signal_connect(shuffle_button, "clicked", G_CALLBACK(on_shuffle_clicked), &app_data);
    g_signal_connect(repeat_button, "clicked", G_CALLBACK(on_repeat_clicked), &app_data);
 
    g_signal_connect(sleep_button, "clicked", G_CALLBACK(on_sleep_clicked), &app_data);
    g_signal_connect(dispupdate_button, "clicked", G_CALLBACK(on_displayUpdate_clicked), &app_data);
    g_signal_connect(frontlight_button, "clicked", G_CALLBACK(on_fl_clicked), &app_data);
    g_signal_connect(bluetooth_button, "clicked", G_CALLBACK(on_bluetooth_clicked), &app_data);
    g_signal_connect(info_button, "clicked", G_CALLBACK(on_info_clicked), &app_data);
    g_signal_connect(background_button, "clicked", G_CALLBACK(on_background_clicked), &app_data);
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_close_clicked), &app_data);

    gtk_box_pack_start(GTK_BOX(controls_hbox), prev_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls_hbox), play_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls_hbox), stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls_hbox), next_button, FALSE, FALSE, 0);
    
    GtkWidget *spacer1 = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(controls_hbox), spacer1, TRUE, TRUE, 0);
    
    GtkWidget *center_hbox = gtk_hbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(center_hbox), shuffle_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center_hbox), repeat_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls_hbox), center_hbox, FALSE, FALSE, 0);
    
    GtkWidget *spacer2 = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(controls_hbox), spacer2, TRUE, TRUE, 0);

    // Volume Slider
    GdkPixbuf *vol_pixbuf = gdk_pixbuf_new_from_inline(-1, app_data.is_hires ? volume_slider_icon : volume_slider_icon_lr, FALSE, NULL);
    GtkWidget *vol_icon = gtk_image_new_from_pixbuf(vol_pixbuf);
    g_object_unref(vol_pixbuf);
    gtk_box_pack_start(GTK_BOX(controls_hbox), vol_icon, FALSE, FALSE, 5);

    app_data.volume_slider = gtk_hscale_new_with_range(0, 1, 0.05);
    gtk_scale_set_draw_value(GTK_SCALE(app_data.volume_slider), FALSE);
    gtk_widget_set_size_request(app_data.volume_slider, app_data.is_hires ? 200 : 100, app_data.is_hires ? 100 : 30);
    gtk_range_set_value(GTK_RANGE(app_data.volume_slider), 1.0);
    g_signal_connect(app_data.volume_slider, "value-changed", G_CALLBACK(on_volume_changed), &app_data);
    gtk_box_pack_start(GTK_BOX(controls_hbox), app_data.volume_slider, FALSE, FALSE, 5);


    GtkWidget *playlist_label = gtk_label_new("<b>Playlist</b>"); 
    gtk_label_set_use_markup(GTK_LABEL(playlist_label), TRUE);
    gtk_box_pack_start(GTK_BOX(main_vbox), playlist_label, FALSE, FALSE, 5); 

    GtkWidget *playlist_frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(playlist_frame), GTK_SHADOW_IN);
    gtk_box_pack_start(GTK_BOX(main_vbox), playlist_frame, TRUE, TRUE, 0);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(playlist_frame), scrolled_window);

    // Initialize stores
    app_data.playlist_store = gtk_list_store_new(1, G_TYPE_STRING);
    app_data.radio_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);

    GtkWidget *playlist_treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app_data.playlist_store));
    app_data.playlist_treeview = GTK_TREE_VIEW(playlist_treeview);
    gtk_container_add(GTK_CONTAINER(scrolled_window), playlist_treeview);

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Filename", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_treeview), column);

    // Container for all bottom actions
    GtkWidget *bottom_action_hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(main_vbox), bottom_action_hbox, FALSE, FALSE, 5);

    // --- Switch Mode Button ---
    app_data.switch_mode_button = create_button_from_icon(app_data.is_hires ? radio_icon : radio_icon_lr, btn_padding);
    g_signal_connect(app_data.switch_mode_button, "clicked", G_CALLBACK(on_switch_mode_clicked), &app_data);
    gtk_box_pack_start(GTK_BOX(bottom_action_hbox), app_data.switch_mode_button, FALSE, FALSE, 0);

    GtkWidget *mode_separator = gtk_vseparator_new();
    gtk_box_pack_start(GTK_BOX(bottom_action_hbox), mode_separator, FALSE, FALSE, 5);

    // --- Music Action HBox ---
    app_data.music_action_hbox = gtk_hbox_new(FALSE, 10);
    gtk_box_pack_start(GTK_BOX(bottom_action_hbox), app_data.music_action_hbox, FALSE, FALSE, 0);

    GtkWidget *add_file_button = create_button_from_icon(app_data.is_hires ? song_add_icon : song_add_icon_lr, btn_padding);
    GtkWidget *add_folder_button = create_button_from_icon(app_data.is_hires ? folder_add_icon : folder_add_icon_lr, btn_padding);
    GtkWidget *clear_playlist_button = create_button_from_icon(app_data.is_hires ? playlist_clear_icon : playlist_clear_icon_lr, btn_padding);
/*    GtkWidget *save_button = gtk_button_new_with_label("Save");
    gtk_container_set_border_width(GTK_CONTAINER(save_button), 5);
    GtkWidget *load_button = gtk_button_new_with_label("Load");
    gtk_container_set_border_width(GTK_CONTAINER(load_button), 5);*/

    g_signal_connect(add_file_button, "clicked", G_CALLBACK(on_add_file_clicked), &app_data);
    g_signal_connect(add_folder_button, "clicked", G_CALLBACK(on_add_folder_clicked), &app_data);
    g_signal_connect(clear_playlist_button, "clicked", G_CALLBACK(on_clear_playlist_clicked), &app_data);
/*    g_signal_connect(save_button, "clicked", G_CALLBACK(on_save_clicked), &app_data);
    g_signal_connect(load_button, "clicked", G_CALLBACK(on_load_clicked), &app_data);*/

    gtk_box_pack_start(GTK_BOX(app_data.music_action_hbox), add_file_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app_data.music_action_hbox), add_folder_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app_data.music_action_hbox), clear_playlist_button, FALSE, FALSE, 0);

/*    GtkWidget *align_save_load = gtk_alignment_new(1, 0.5, 0, 0);
    GtkWidget *save_load_hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(save_load_hbox), save_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(save_load_hbox), load_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(align_save_load), save_load_hbox);
    gtk_box_pack_start(GTK_BOX(app_data.music_action_hbox), align_save_load, TRUE, TRUE, 0);*/

    // --- Radio Action HBox (Initially Hidden) ---
    app_data.radio_action_hbox = gtk_hbox_new(FALSE, 10);
    gtk_box_pack_start(GTK_BOX(bottom_action_hbox), app_data.radio_action_hbox, FALSE, FALSE, 0);

    GtkWidget *radio_info_label = gtk_label_new("Add radio stations in KUAL.");
    gtk_box_pack_start(GTK_BOX(app_data.radio_action_hbox), radio_info_label, FALSE, FALSE, 0);

    // --- Right controls (Common to both modes) ---
    GtkWidget *right_controls_hbox = gtk_hbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), sleep_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), dispupdate_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), frontlight_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), bluetooth_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), info_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), background_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), close_button, FALSE, FALSE, 0);

    GtkWidget *align_right_controls = gtk_alignment_new(1, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(align_right_controls), right_controls_hbox);
    gtk_box_pack_start(GTK_BOX(bottom_action_hbox), align_right_controls, TRUE, TRUE, 0);
/*    GtkWidget *add_station_button = gtk_button_new_with_label("Add station");
    gtk_container_set_border_width(GTK_CONTAINER(add_station_button), 5);
    GtkWidget *remove_station_button = gtk_button_new_with_label("Remove selected");
    gtk_container_set_border_width(GTK_CONTAINER(remove_station_button), 5);

    g_signal_connect(add_station_button, "clicked", G_CALLBACK(on_add_station_clicked), &app_data);
    g_signal_connect(remove_station_button, "clicked", G_CALLBACK(on_remove_station_clicked), &app_data);

    gtk_box_pack_start(GTK_BOX(app_data.radio_action_hbox), add_station_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app_data.radio_action_hbox), remove_station_button, FALSE, FALSE, 0);*/


    load_radio_stations(&app_data);
    load_state(&app_data);
    
    gtk_widget_show_all(window);
    
    // Ensure the correct mode UI is shown after show_all
    if (app_data.is_radio_mode) {
        gtk_widget_hide(app_data.music_action_hbox);
        gtk_widget_show(app_data.radio_action_hbox);
    } else {
        gtk_widget_show(app_data.music_action_hbox);
        gtk_widget_hide(app_data.radio_action_hbox);
    }

    g_timeout_add(500, update_progress_cb, &app_data);
    
    gtk_main();

    return 0;
}

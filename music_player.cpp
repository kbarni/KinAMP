#include <gtk/gtk.h>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <stdlib.h>
#include <wordexp.h>
#include <libgen.h>
#include <random>

#include "openlipc/openlipc.h"

#include "gtk_utils.h"
#include "music_backend.h"
#include "assets/bluetooth_icon.h"
#include "assets/close_icon.h"
#include "assets/play_pause_icon.h"
#include "assets/skip_next_icon.h"
#include "assets/skip_previous_icon.h"
#include "assets/stop_icon.h"
#include "assets/title_icon.h"
#include "assets/title_christmas.h"
#include "assets/shuffle_icon.h"
#include "assets/repeat_icon.h"
#include "assets/shuffle_on_icon.h"
#include "assets/repeat_on_icon.h"
#include "assets/sunny_icon.h"

enum PlaybackStrategy {
    NORMAL,
    REPEAT,
    RANDOM
};

struct AppData {
    MusicBackend *backend;
    GtkListStore *playlist_store;
    GtkTreeView *playlist_treeview;
    GtkLabel *song_title_label;
    GtkLabel *time_label;

    PlaybackStrategy current_strategy;
    int flIntensity;
    bool next_song_pending;
    std::string next_song_path;
    std::string last_title; // Cache to avoid redundant UI updates
    GtkWidget *shuffle_button;
    GtkWidget *repeat_button;
};

static LIPC * lipcInstance = 0;

void openLipcInstance() {
	if (lipcInstance == 0) {
		fwprintf(stderr, L"---->openLipcInstance()\n");
		lipcInstance = LipcOpen("com.kbarni.kinamp");
	}
}

void closeLipcInstance() {
	if (lipcInstance != 0) {
		fwprintf(stderr, L"---->closeLipcInstance()\n");
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

void set_button_icon(GtkWidget *button, const unsigned char *icon_data) {
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_inline(-1, icon_data, FALSE, NULL);
    GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    gtk_button_set_image(GTK_BUTTON(button), image);
    gtk_widget_show(image); // Important to show the new image
}

// --- End of Stream Callback ---
void on_eos_cb(void* user_data) {
    g_print("UI: End-of-Stream reached. Planning next song.\n");
    AppData *app_data = (AppData*)user_data;

    GtkTreeModel *model = GTK_TREE_MODEL(app_data->playlist_store);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(app_data->playlist_treeview);
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
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
    }

    gtk_tree_path_free(current_path);
}


// --- UI Update Callback ---
gboolean update_progress_cb(gpointer data) {
    AppData *app_data = (AppData*)data;

    // Handle playing the next song if pending
    if (app_data->next_song_pending && !app_data->backend->is_playing && !app_data->backend->is_shutting_down()) {
        app_data->next_song_pending = false;
        
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app_data->playlist_store), &iter);
        while (valid) {
            gchar *path = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(app_data->playlist_store), &iter, 0, &path, -1);
            // Use std::string comparison
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
        return TRUE; // Return early
    }


    if (app_data->backend->is_playing || app_data->backend->is_paused) {
        gint64 position = app_data->backend->get_position();
        
        char time_str[32];
        int pos_seconds = position / GST_SECOND;
        
        if (app_data->backend->is_paused) {
             snprintf(time_str, sizeof(time_str), "|| %02d:%02d",
                     pos_seconds / 60, pos_seconds % 60);
        } else {
             snprintf(time_str, sizeof(time_str), " > %02d:%02d",
                     pos_seconds / 60, pos_seconds % 60);
        }
        gtk_label_set_text(app_data->time_label, time_str);
        
        // Update title only if it changes
        const char* full_path = app_data->backend->get_current_filepath();
        if (full_path && strlen(full_path) > 0) {
            char* path_copy = g_strdup(full_path);
            char* base = basename(path_copy); // basename might modify path_copy
            
            if (app_data->last_title != base) {
                gtk_label_set_text(app_data->song_title_label, base);
                app_data->last_title = base; // Update cache
            }
            g_free(path_copy);
        }

    } else {
        // Stopped state
        gtk_label_set_text(app_data->time_label, "[] 00:00");
        if (app_data->last_title != "No song playing") {
            gtk_label_set_text(app_data->song_title_label, "No song playing");
            app_data->last_title = "No song playing";
        }
    }

    return TRUE; // Continue calling this function
}


// --- Helper to get config file path ---
std::string get_config_path(const char* filename) {
    std::string path;
    wordexp_t p;
    if (wordexp("~", &p, 0) == 0) {
        if (p.we_wordv[0]) {
            path = p.we_wordv[0];
            path += "/";
            path += filename;
        }
        wordfree(&p);
    } 
    
    if (path.empty()) {
        // Fallback to current directory
        path = filename;
    }
    return path;
}


// --- State Management ---
void save_state(AppData *app_data) {
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
        conffile << "current_index=" << current_index << std::endl;
        conffile << "playback_strategy=" << app_data->current_strategy << std::endl;
        conffile.close();
    }
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
            if (line.find("current_index=") == 0) {
                current_index = atoi(line.substr(14).c_str());
            }
            if (line.find("playback_strategy=") == 0) {
                int strategy = atoi(line.substr(20).c_str());
                app_data->current_strategy = (PlaybackStrategy)strategy;

                if (app_data->current_strategy == RANDOM) {
                    set_button_icon(app_data->shuffle_button, shuffle_on_icon);
                    set_button_icon(app_data->repeat_button, repeat_icon);
                } else if (app_data->current_strategy == REPEAT) {
                    set_button_icon(app_data->shuffle_button, shuffle_icon);
                    set_button_icon(app_data->repeat_button, repeat_on_icon);
                } else {
                    set_button_icon(app_data->shuffle_button, shuffle_icon);
                    set_button_icon(app_data->repeat_button, repeat_icon);
                }
            }
        }
        conffile.close();
    }

    if (current_index != -1) {
        GtkTreePath *path = gtk_tree_path_new_from_indices(current_index, -1);
        if (path) {
            gtk_tree_view_set_cursor(app_data->playlist_treeview, path, NULL, FALSE);
            gtk_tree_path_free(path);
        }
    }
}


// --- Function to set font for a label ---
void set_label_font(GtkWidget *label, const char *font_desc_str) {
    PangoFontDescription *font_desc = pango_font_description_from_string(font_desc_str);
    gtk_widget_modify_font(label, font_desc);
    pango_font_description_free(font_desc);
}

// --- Helper function to recursively add files from a directory ---
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

// --- Playback Controls ---
void play_selected_song(AppData* app_data) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(app_data->playlist_treeview);
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gchar *file_path = NULL;
        gtk_tree_model_get(model, &iter, 0, &file_path, -1);
        if (file_path) {
            app_data->backend->play_file(file_path);
            
            // Update title immediately on play
            char* path_copy = g_strdup(file_path);
            char* base = basename(path_copy);
            gtk_label_set_text(app_data->song_title_label, base);
            app_data->last_title = base; // Update cache
            
            g_free(path_copy);
            g_free(file_path);
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
        gtk_tree_path_next(path); // Move to the next path
        
        // Check if the path is valid before using it
        if (gtk_tree_model_get_iter(model, &iter, path)) {
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(app_data->playlist_treeview), path, NULL, FALSE);
            play_selected_song(app_data);
        }
        gtk_tree_path_free(path);
    }
}

void on_close_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    LipcSetIntProperty(lipcInstance,"com.lab126.powerd","flIntensity",app_data->flIntensity);
    enableSleep();
    closeLipcInstance();
    AppData *app_data = (AppData*)data;
    save_state(app_data);
    app_data->backend->stop();
    gtk_main_quit();
}

void on_shuffle_clicked(GtkWidget *widget, gpointer data) {
    AppData *app_data = (AppData*)data;
    if (app_data->current_strategy == RANDOM) {
        app_data->current_strategy = NORMAL;
        set_button_icon(widget, shuffle_icon);
    } else {
        app_data->current_strategy = RANDOM;
        set_button_icon(widget, shuffle_on_icon);
        // Turn off repeat
        set_button_icon(app_data->repeat_button, repeat_icon);
    }
    g_print("Shuffle mode toggled. New strategy: %d\n", app_data->current_strategy);
}

void on_repeat_clicked(GtkWidget *widget, gpointer data) {
    AppData *app_data = (AppData*)data;
    if (app_data->current_strategy == REPEAT) {
        app_data->current_strategy = NORMAL;
        set_button_icon(widget, repeat_icon);
    } else {
        app_data->current_strategy = REPEAT;
        set_button_icon(widget, repeat_on_icon);
        // Turn off shuffle
        set_button_icon(app_data->shuffle_button, shuffle_icon);
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


// --- Playlist Management Callbacks ---
void on_add_file_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app_data = (AppData*)data;
    GtkListStore *playlist_store = app_data->playlist_store;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("L:D_N:dialog_PC:TS_ID:com.kbarni.kinamp",
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
    GtkWidget *dialog = gtk_file_chooser_dialog_new("L:D_N:dialog_PC:TS_ID:com.kbarni.kinamp",
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
    GtkWidget *dialog = gtk_file_chooser_dialog_new("L:D_N:dialog_PC:TS_ID:com.kbarni.kinamp",
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
    GtkWidget *dialog = gtk_file_chooser_dialog_new("L:D_N:dialog_PC:TS_ID:com.kbarni.kinamp",
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


int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    // --- App Data ---
    MusicBackend backend;
    AppData app_data;
    app_data.backend = &backend;
    app_data.current_strategy = NORMAL;
    app_data.next_song_pending = false;
    app_data.flIntensity = 0;

    backend.set_eos_callback(on_eos_cb, &app_data);

    openLipcInstance();
    disableSleep();

    // --- Main Window ---
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 400);
    gtk_window_set_title(GTK_WINDOW(window), "L:A_N:application_PC:T_ID:com.kbarni.kinamp");
    g_signal_connect(window, "destroy", G_CALLBACK(on_close_clicked), &app_data);

    // --- Main Vertical Box ---
    GtkWidget *main_vbox = gtk_vbox_new(FALSE, 10); // Increased spacing
    gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 20); // Add margins
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    // --- Player Section ---
    GtkWidget *player_vbox = gtk_vbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(main_vbox), player_vbox, FALSE, FALSE, 0);

    // First line: Title Image
    GdkPixbuf *title_pixbuf = gdk_pixbuf_new_from_inline(-1, title_christmas, FALSE, NULL);
    GtkWidget *title_image = gtk_image_new_from_pixbuf(title_pixbuf);
    g_object_unref(title_pixbuf); // GtkImage takes a copy or ref? documentation says it takes a ref, but usually safe to unref if not needed else. 
    // Actually, create_from_pixbuf adds a ref. We should unref ours.
    // However, pixbuf_new_from_inline might need care.
    // Let's rely on standard behavior: GtkImage references it.
    
    // Center the title image
    GtkWidget *title_alignment = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(title_alignment), title_image);
    gtk_box_pack_start(GTK_BOX(player_vbox), title_alignment, FALSE, FALSE, 0);

    // Time and Song Title
    GtkWidget *info_hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(player_vbox), info_hbox, FALSE, FALSE, 0);

    // Time Label
    GtkWidget *time_label = gtk_label_new("[] 00:00");
    app_data.time_label = GTK_LABEL(time_label);
    set_label_font(time_label, "Sans Bold 20");
    GtkWidget *time_frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(time_frame), time_label);
    gtk_box_pack_start(GTK_BOX(info_hbox), time_frame, FALSE, FALSE, 0);

    // Song Title Label
    GtkWidget *song_title_label = gtk_label_new("No song playing");
    app_data.song_title_label = GTK_LABEL(song_title_label);
    set_label_font(song_title_label, "Sans 14");
    gtk_box_pack_start(GTK_BOX(info_hbox), song_title_label, TRUE, TRUE, 0);

    // Separator line (replaces progress bar)
    GtkWidget *separator = gtk_hseparator_new();
    gtk_widget_set_size_request(separator, -1, 10); // Give it some vertical space
    gtk_box_pack_start(GTK_BOX(player_vbox), separator, FALSE, FALSE, 5);

    // Control Buttons
    GtkWidget *controls_hbox = gtk_hbox_new(FALSE, 2); 
    gtk_box_pack_start(GTK_BOX(player_vbox), controls_hbox, FALSE, FALSE, 0);

    GtkWidget *prev_button = create_button_from_icon(skip_previous_icon, 72, 72, 5);
    GtkWidget *play_button = create_button_from_icon(play_pause_icon, 72, 72, 5);
    GtkWidget *stop_button = create_button_from_icon(stop_icon, 72, 72, 5);
    GtkWidget *next_button = create_button_from_icon(skip_next_icon, 72, 72, 5);
    
    GtkWidget *shuffle_button = create_button_from_icon(shuffle_icon, 72, 72, 5);
    app_data.shuffle_button = shuffle_button;
    GtkWidget *repeat_button = create_button_from_icon(repeat_icon, 72, 72, 5);
    app_data.repeat_button = repeat_button;

    GtkWidget *frontlight_button = create_button_from_icon(sunny_icon, 72, 72, 5);
    GtkWidget *bluetooth_button = create_button_from_icon(bluetooth_icon, 72, 72, 5);
    GtkWidget *close_button = create_button_from_icon(close_icon, 72, 72, 5);

    g_signal_connect(prev_button, "clicked", G_CALLBACK(on_previous_clicked), &app_data);
    g_signal_connect(play_button, "clicked", G_CALLBACK(on_play_pause_clicked), &app_data);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_stop_clicked), &app_data);
    g_signal_connect(next_button, "clicked", G_CALLBACK(on_next_clicked), &app_data);
    
    g_signal_connect(shuffle_button, "clicked", G_CALLBACK(on_shuffle_clicked), &app_data);
    g_signal_connect(repeat_button, "clicked", G_CALLBACK(on_repeat_clicked), &app_data);
 
    g_signal_connect(frontlight_button, "clicked", G_CALLBACK(on_fl_clicked), &app_data);
    g_signal_connect(bluetooth_button, "clicked", G_CALLBACK(on_bluetooth_clicked), &app_data);
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_close_clicked), &app_data);

    // Pack buttons
    gtk_box_pack_start(GTK_BOX(controls_hbox), prev_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls_hbox), play_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls_hbox), stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls_hbox), next_button, FALSE, FALSE, 0);
    
    // Spacer
    GtkWidget *spacer1 = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(controls_hbox), spacer1, TRUE, TRUE, 0);
    
    // Center buttons
    GtkWidget *center_hbox = gtk_hbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(center_hbox), shuffle_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center_hbox), repeat_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls_hbox), center_hbox, FALSE, FALSE, 0);
    
    // Spacer
    GtkWidget *spacer2 = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(controls_hbox), spacer2, TRUE, TRUE, 0);

    // Right aligned section (Bluetooth + Close)
    GtkWidget *right_controls_hbox = gtk_hbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), frontlight_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), bluetooth_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_controls_hbox), close_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls_hbox), right_controls_hbox, FALSE, FALSE, 0);


    // --- Playlist Section ---
    GtkWidget *playlist_label = gtk_label_new("<b>Playlist</b>"); 
    gtk_label_set_use_markup(GTK_LABEL(playlist_label), TRUE);
    gtk_box_pack_start(GTK_BOX(main_vbox), playlist_label, FALSE, FALSE, 5); 

    GtkWidget *playlist_frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(playlist_frame), GTK_SHADOW_IN);
    gtk_box_pack_start(GTK_BOX(main_vbox), playlist_frame, TRUE, TRUE, 0);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(playlist_frame), scrolled_window);

    GtkListStore *playlist_store = gtk_list_store_new(1, G_TYPE_STRING);
    app_data.playlist_store = playlist_store;
    GtkWidget *playlist_treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(playlist_store));
    app_data.playlist_treeview = GTK_TREE_VIEW(playlist_treeview);
    gtk_container_add(GTK_CONTAINER(scrolled_window), playlist_treeview);

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Filename", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(playlist_treeview), column);


    // --- Playlist Management Buttons ---
    GtkWidget *playlist_buttons_hbox = gtk_hbox_new(FALSE, 10); // Increased spacing
    gtk_box_pack_start(GTK_BOX(main_vbox), playlist_buttons_hbox, FALSE, FALSE, 5);

    GtkWidget *add_file_button = gtk_button_new_with_label("Add file");
    gtk_container_set_border_width(GTK_CONTAINER(add_file_button), 5);
    GtkWidget *add_folder_button = gtk_button_new_with_label("Add Folder");
    gtk_container_set_border_width(GTK_CONTAINER(add_folder_button), 5);
    GtkWidget *clear_playlist_button = gtk_button_new_with_label("Clear playlist");
    gtk_container_set_border_width(GTK_CONTAINER(clear_playlist_button), 5);
    GtkWidget *save_button = gtk_button_new_with_label("Save");
    gtk_container_set_border_width(GTK_CONTAINER(save_button), 5);
    GtkWidget *load_button = gtk_button_new_with_label("Load");
    gtk_container_set_border_width(GTK_CONTAINER(load_button), 5);

    g_signal_connect(add_file_button, "clicked", G_CALLBACK(on_add_file_clicked), &app_data);
    g_signal_connect(add_folder_button, "clicked", G_CALLBACK(on_add_folder_clicked), &app_data);
    g_signal_connect(clear_playlist_button, "clicked", G_CALLBACK(on_clear_playlist_clicked), &app_data);
    g_signal_connect(save_button, "clicked", G_CALLBACK(on_save_clicked), &app_data);
    g_signal_connect(load_button, "clicked", G_CALLBACK(on_load_clicked), &app_data);

    gtk_box_pack_start(GTK_BOX(playlist_buttons_hbox), add_file_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(playlist_buttons_hbox), add_folder_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(playlist_buttons_hbox), clear_playlist_button, FALSE, FALSE, 0);

    // Align Save and Load buttons to the right
    GtkWidget *align_save_load = gtk_alignment_new(1, 0.5, 0, 0);
    GtkWidget *save_load_hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(save_load_hbox), save_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(save_load_hbox), load_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(align_save_load), save_load_hbox);
    gtk_box_pack_start(GTK_BOX(playlist_buttons_hbox), align_save_load, TRUE, TRUE, 0);


    // --- Load State and Show Window ---
    load_state(&app_data);
    g_timeout_add(200, update_progress_cb, &app_data);
    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
#ifndef RADIODIALOG_HPP
#define RADIODIALOG_HPP

// Radio station manager, in the player itself. Lists the stations, adds one
// from the bundled database or by hand, removes one.
// Text is typed on the on-screen keyboard (keyboarddialog.hpp) because the
// Kindle's own keyboard cannot be raised over a native GTK window.
//
// The dialog edits the player's station store in place and calls back after
// every change so the caller can write `.kinamp_radio.txt`.

#include <gtk/gtk.h>
#include <cstdarg>
#include <string>
#include <vector>

#include "keyboarddialog.hpp"
#include "station_db.h"

// Station store columns, matching the player's radio_store.
#define STATION_COL_NAME 0
#define STATION_COL_URL  1

// A search that matches half the database is not worth listing on an e-ink
// screen; the user narrows the term instead.
#define STATION_SEARCH_LIMIT 200

typedef void (*StationsChangedFn)(void *user_data);

// ------------------------------------------------------------------ helpers

static void station_message(GtkWindow *parent, GtkMessageType type, const char *format, ...)
    G_GNUC_PRINTF(3, 4);

static void station_message(GtkWindow *parent, GtkMessageType type, const char *format, ...) {
    va_list args;
    va_start(args, format);
    gchar *text = g_strdup_vprintf(format, args);
    va_end(args);

    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type,
                                               GTK_BUTTONS_OK, "%s", text);
    gtk_window_set_title(GTK_WINDOW(dialog), "L:D_N:dialog_ID:com.kbarni.kinamp");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(text);
}

static bool station_confirm(GtkWindow *parent, const char *format, ...) G_GNUC_PRINTF(2, 3);

static bool station_confirm(GtkWindow *parent, const char *format, ...) {
    va_list args;
    va_start(args, format);
    gchar *text = g_strdup_vprintf(format, args);
    va_end(args);

    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_YES_NO, "%s", text);
    gtk_window_set_title(GTK_WINDOW(dialog), "L:D_N:dialog_ID:com.kbarni.kinamp");
    gint answer = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(text);
    return answer == GTK_RESPONSE_YES;
}

// Downloading blocks the main loop for a few seconds, so say what is going on
// and let GTK paint it before the call that stalls.
static GtkWidget *station_busy_show(GtkWindow *parent, const char *text) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "L:D_N:dialog_ID:com.kbarni.kinamp");
    gtk_window_set_transient_for(GTK_WINDOW(window), parent);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_container_set_border_width(GTK_CONTAINER(window), 30);

    GtkWidget *label = gtk_label_new(text);
    gtk_container_add(GTK_CONTAINER(window), label);
    gtk_widget_show_all(window);

    while (gtk_events_pending()) gtk_main_iteration();
    return window;
}

static void station_busy_hide(GtkWidget *window) {
    if (!window) return;
    gtk_widget_destroy(window);
    while (gtk_events_pending()) gtk_main_iteration();
}

static void on_station_row_activated(GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *,
                                     gpointer user_data) {
    gtk_dialog_response(GTK_DIALOG(user_data), GTK_RESPONSE_OK);
}

// Modal list picker used for search results and for the streams inside a
// playlist. `subtitles` may be NULL. Returns the chosen index, or -1.
// `selected` is the row to start on and receives the row that was picked, so
// the list can be reopened where the user left it.
static int station_pick_from_list(GtkWindow *parent, const char *title, const char *heading,
                                  const std::vector<std::string> &titles,
                                  const std::vector<std::string> *subtitles,
                                  int *selected) {
    GdkScreen *screen = gdk_screen_get_default();
    gint width = gdk_screen_get_width(screen);
    gint height = gdk_screen_get_height(screen);
    bool is_small = (width < 1000);

    GtkWidget *dialog = gtk_dialog_new_with_buttons("L:D_N:dialog_ID:com.kbarni.kinamp",
                                                    parent, GTK_DIALOG_MODAL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), width - 40, height - 120);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), is_small ? 10 : 20);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_vbox_new(FALSE, is_small ? 5 : 10);
    gtk_container_add(GTK_CONTAINER(content_area), vbox);

    gchar *markup = g_markup_printf_escaped("<b><big>%s</big></b>", title);
    GtkWidget *title_label = gtk_label_new(markup);
    g_free(markup);
    gtk_label_set_use_markup(GTK_LABEL(title_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), title_label, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_hseparator_new(), FALSE, FALSE, 0);

    if (heading && *heading) {
        GtkWidget *heading_label = gtk_label_new(heading);
        gtk_misc_set_alignment(GTK_MISC(heading_label), 0.0, 0.5);
        gtk_box_pack_start(GTK_BOX(vbox), heading_label, FALSE, FALSE, 2);
    }

    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    for (size_t i = 0; i < titles.size(); ++i) {
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                           0, titles[i].c_str(),
                           1, (subtitles && i < subtitles->size()) ? (*subtitles)[i].c_str() : "",
                           -1);
    }

    GtkWidget *treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    g_signal_connect(treeview, "row-activated", G_CALLBACK(on_station_row_activated), dialog);

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(treeview), -1, "Name",
                                                renderer, "text", 0, NULL);
    if (subtitles) {
        GtkCellRenderer *sub_renderer = gtk_cell_renderer_text_new();
        g_object_set(sub_renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(treeview), -1, "Stream",
                                                    sub_renderer, "text", 1, NULL);
    }

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled), treeview);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    if (selected && *selected >= 0 && (size_t)*selected < titles.size()) {
        GtkTreePath *path = gtk_tree_path_new_from_indices(*selected, -1);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
        gtk_tree_path_free(path);
    }

    GtkWidget *ok_button = gtk_dialog_add_button(GTK_DIALOG(dialog), "Select", GTK_RESPONSE_OK);
    GtkWidget *cancel_button = gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_widget_set_size_request(ok_button, is_small ? 120 : 200, is_small ? 42 : 64);
    gtk_widget_set_size_request(cancel_button, is_small ? 120 : 200, is_small ? 42 : 64);

    gtk_widget_show_all(dialog);

    int result = -1;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
        GtkTreeModel *model;
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            gint *indices = gtk_tree_path_get_indices(path);
            if (indices) result = indices[0];
            gtk_tree_path_free(path);
        }
    }
    if (selected && result >= 0) *selected = result;

    gtk_widget_destroy(dialog);
    return result;
}

// ------------------------------------------------------- adding a station

// stationdb calls this when a playlist holds more than one entry.
static bool station_choose_stream(const std::vector<std::string> &streams,
                                  size_t &index, void *user_data) {
    GtkWindow *parent = GTK_WINDOW(user_data);

    std::vector<std::string> notes;
    notes.reserve(streams.size());
    for (size_t i = 0; i < streams.size(); ++i) {
        notes.push_back(stationdb::is_unsupported_stream(streams[i]) ? "unsupported" : "");
    }

    int selected = 0;
    int choice = station_pick_from_list(parent, "Select stream",
                                        "This station points at a playlist:",
                                        streams, &notes, &selected);
    if (choice < 0) return false;
    index = (size_t)choice;
    return true;
}

// Unwraps playlists and rejects what the player cannot decode. False means
// the station should not be added (cancelled, or reported to the user here).
static bool station_resolve(GtkWindow *parent, Station &station) {
    GtkWidget *busy = NULL;
    if (stationdb::needs_resolving(station.url)) {
        busy = station_busy_show(parent, "Resolving stream address...");
    }

    std::string error;
    stationdb::ResolveResult result =
        stationdb::resolve_playlist_url(station, station_choose_stream, parent, &error);
    station_busy_hide(busy);

    if (result == stationdb::RESOLVE_CANCELLED) return false;
    if (result == stationdb::RESOLVE_ERROR) {
        station_message(parent, GTK_MESSAGE_ERROR, "%s", error.c_str());
        return false;
    }
    // Re-check after resolving: a playlist can point at a format the player
    // cannot decode yet.
    if (stationdb::is_unsupported_stream(station.url)) {
        station_message(parent, GTK_MESSAGE_WARNING,
                        "HLS (.m3u8) streams are not supported yet.");
        return false;
    }
    return true;
}

static void station_append(GtkListStore *store, GtkTreeView *view, const Station &station) {
    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter,
                       STATION_COL_NAME, station.name.c_str(),
                       STATION_COL_URL, station.url.c_str(), -1);

    if (view) {
        GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &iter);
        gtk_tree_view_set_cursor(view, path, NULL, FALSE);
        gtk_tree_view_scroll_to_cell(view, path, NULL, TRUE, 1.0, 0.0);
        gtk_tree_path_free(path);
    }
}

// Search the bundled database, then add what the user picks.
static void station_add_from_database(GtkWindow *parent, GtkListStore *store, GtkTreeView *view,
                                      StationsChangedFn on_changed, void *user_data) {
    std::string term;
    if (!show_keyboard_dialog(parent, "Add station", "Search term:", term, 40, false)) return;
    if (term.empty()) return;

    GtkWidget *busy = station_busy_show(parent, "Searching the station database...");
    std::vector<Station> found;
    bool truncated = false;
    bool ok = stationdb::search(term, found, STATION_SEARCH_LIMIT, &truncated);
    station_busy_hide(busy);

    if (!ok) {
        station_message(parent, GTK_MESSAGE_ERROR,
                        "The station database (allStations.json) was not found\n"
                        "next to the KinAMP binaries.");
        return;
    }
    if (found.empty()) {
        station_message(parent, GTK_MESSAGE_INFO, "No stations found matching '%s'.", term.c_str());
        return;
    }

    std::vector<std::string> names, urls;
    names.reserve(found.size());
    urls.reserve(found.size());
    for (size_t i = 0; i < found.size(); ++i) {
        names.push_back(found[i].name);
        urls.push_back(found[i].url);
    }

    std::string heading = truncated
        ? "First " + std::to_string(found.size()) + " matches - narrow the search for more"
        : std::to_string(found.size()) + (found.size() == 1 ? " match" : " matches");

    // Stay on the result list when a station turns out to be unusable.
    int selected = 0;
    while (true) {
        int choice = station_pick_from_list(parent, "Search results", heading.c_str(),
                                            names, &urls, &selected);
        if (choice < 0) return;

        Station station = found[choice];
        if (!station_resolve(parent, station)) continue;

        station_append(store, view, station);
        if (on_changed) on_changed(user_data);
        return;
    }
}

// Name and URL typed by hand.
static void station_add_manually(GtkWindow *parent, GtkListStore *store, GtkTreeView *view,
                                 StationsChangedFn on_changed, void *user_data) {
    std::string name;
    if (!show_keyboard_dialog(parent, "Add station manually", "Station name:", name, 40)) return;
    if (name.empty()) return;

    std::string url = "http://";
    if (!show_keyboard_dialog(parent, "Add station manually", "Stream URL:", url, 300, false)) return;
    if (url.empty() || url == "http://") return;

    Station station;
    station.name = name;
    station.url = url;
    if (!station_resolve(parent, station)) return;

    station_append(store, view, station);
    if (on_changed) on_changed(user_data);
}

// --------------------------------------------------------- the manager dialog

struct StationManagerData {
    GtkWindow *parent;
    GtkWidget *dialog;
    GtkListStore *store;
    GtkTreeView *view;
    StationsChangedFn on_changed;
    void *user_data;
};

static void on_station_add_db_clicked(GtkWidget *, gpointer user_data) {
    StationManagerData *md = (StationManagerData*)user_data;
    station_add_from_database(GTK_WINDOW(md->dialog), md->store, md->view,
                              md->on_changed, md->user_data);
}

static void on_station_add_manual_clicked(GtkWidget *, gpointer user_data) {
    StationManagerData *md = (StationManagerData*)user_data;
    station_add_manually(GTK_WINDOW(md->dialog), md->store, md->view,
                         md->on_changed, md->user_data);
}

static void on_station_remove_clicked(GtkWidget *, gpointer user_data) {
    StationManagerData *md = (StationManagerData*)user_data;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(md->view);
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        station_message(GTK_WINDOW(md->dialog), GTK_MESSAGE_INFO,
                        "Select the station to remove first.");
        return;
    }

    gchar *name = NULL;
    gtk_tree_model_get(model, &iter, STATION_COL_NAME, &name, -1);
    bool confirmed = station_confirm(GTK_WINDOW(md->dialog), "Remove '%s'?", name ? name : "");
    g_free(name);
    if (!confirmed) return;

    gtk_list_store_remove(md->store, &iter);
    if (md->on_changed) md->on_changed(md->user_data);
}

// Opens the station manager and blocks until it is closed. `store` is the
// player's station list (name in column 0, URL in column 1); `on_changed` is
// called after every addition or removal so the list can be saved.
static void show_station_manager(GtkWindow *parent, GtkListStore *store,
                                 StationsChangedFn on_changed, void *user_data) {
    GdkScreen *screen = gdk_screen_get_default();
    gint width = gdk_screen_get_width(screen);
    gint height = gdk_screen_get_height(screen);
    bool is_small = (width < 1000);
    int button_height = is_small ? 42 : 64;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("L:D_N:dialog_ID:com.kbarni.kinamp",
                                                    parent, GTK_DIALOG_MODAL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), width - 40, height - 120);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), is_small ? 10 : 20);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_vbox_new(FALSE, is_small ? 5 : 10);
    gtk_container_add(GTK_CONTAINER(content_area), vbox);

    GtkWidget *title_label = gtk_label_new("<b><big>Radio stations</big></b>");
    gtk_label_set_use_markup(GTK_LABEL(title_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), title_label, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_hseparator_new(), FALSE, FALSE, 0);

    // The player's own store, so the list behind the dialog follows along.
    GtkWidget *treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));

    GtkCellRenderer *name_renderer = gtk_cell_renderer_text_new();
    g_object_set(name_renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(treeview), -1, "Station",
                                                name_renderer, "text", STATION_COL_NAME, NULL);
    GtkCellRenderer *url_renderer = gtk_cell_renderer_text_new();
    g_object_set(url_renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(treeview), -1, "Stream",
                                                url_renderer, "text", STATION_COL_URL, NULL);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled), treeview);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    StationManagerData md;
    md.parent = parent;
    md.dialog = dialog;
    md.store = store;
    md.view = GTK_TREE_VIEW(treeview);
    md.on_changed = on_changed;
    md.user_data = user_data;

    GtkWidget *button_hbox = gtk_hbox_new(TRUE, is_small ? 5 : 10);
    gtk_box_pack_start(GTK_BOX(vbox), button_hbox, FALSE, FALSE, 5);

    GtkWidget *search_button = gtk_button_new_with_label("Add from database");
    GtkWidget *manual_button = gtk_button_new_with_label("Add manually");
    GtkWidget *remove_button = gtk_button_new_with_label("Remove");
    gtk_widget_set_size_request(search_button, -1, button_height);
    gtk_widget_set_size_request(manual_button, -1, button_height);
    gtk_widget_set_size_request(remove_button, -1, button_height);

    g_signal_connect(search_button, "clicked", G_CALLBACK(on_station_add_db_clicked), &md);
    g_signal_connect(manual_button, "clicked", G_CALLBACK(on_station_add_manual_clicked), &md);
    g_signal_connect(remove_button, "clicked", G_CALLBACK(on_station_remove_clicked), &md);

    gtk_box_pack_start(GTK_BOX(button_hbox), search_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(button_hbox), manual_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(button_hbox), remove_button, TRUE, TRUE, 0);

    GtkWidget *close_button = gtk_dialog_add_button(GTK_DIALOG(dialog), "Close", GTK_RESPONSE_CLOSE);
    gtk_widget_set_size_request(close_button, is_small ? 120 : 200, button_height);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

#endif // RADIODIALOG_HPP

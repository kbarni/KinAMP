#ifndef KEYBOARDDIALOG_HPP
#define KEYBOARDDIALOG_HPP

// On-screen keyboard dialog. The Kindle's own keyboard cannot be summoned
// reliably from a native GTK window, so text is typed on a grid of buttons
// instead: the entry itself is not editable, which also keeps the system
// keyboard from appearing over the dialog.
//
//     std::string name;
//     if (show_keyboard_dialog(parent, "Add station", "Station name:", name, 40))
//         ... // OK pressed, `name` holds what was typed
//
// Header-only and self-contained: include it wherever the dialog is needed.

#include <gtk/gtk.h>
#include <string>
#include <vector>

// Every window has to carry this title: the Kindle window manager reads it to
// decide the layer a window belongs to, and one it does not recognize is never
// mapped. Defined here so the dialogs cannot drift apart.
#ifndef KINAMP_DIALOG_TITLE
#define KINAMP_DIALOG_TITLE "L:D_N:dialog_ID:com.kbarni.kinamp"
#endif

// ------------------------------------------------------------------ internals

struct KeyboardDialogState {
    GtkWidget *entry;
    size_t max_len;
    bool shift;
    std::vector<GtkWidget*> letter_keys; // relabelled when Shift toggles
};

struct KeyboardKeyData {
    KeyboardDialogState *state;
    char character;
};

static void keyboard_set_text(KeyboardDialogState *state, const std::string &text) {
    gtk_entry_set_text(GTK_ENTRY(state->entry), text.c_str());
    // Keep the tail of a long URL in view.
    gtk_editable_set_position(GTK_EDITABLE(state->entry), -1);
}

static std::string keyboard_get_text(KeyboardDialogState *state) {
    const char *current = gtk_entry_get_text(GTK_ENTRY(state->entry));
    return current ? current : "";
}

static void keyboard_append(KeyboardDialogState *state, char c) {
    std::string text = keyboard_get_text(state);
    if (state->max_len && text.length() >= state->max_len) return;
    text += c;
    keyboard_set_text(state, text);
}

static void on_keyboard_key_clicked(GtkWidget *, gpointer user_data) {
    KeyboardKeyData *kd = (KeyboardKeyData*)user_data;
    char c = kd->character;
    if (c >= 'a' && c <= 'z' && kd->state->shift) c = c - 'a' + 'A';
    keyboard_append(kd->state, c);
}

static void on_keyboard_key_destroy(GtkWidget *, gpointer user_data) {
    g_free(user_data);
}

static void on_keyboard_space_clicked(GtkWidget *, gpointer user_data) {
    keyboard_append((KeyboardDialogState*)user_data, ' ');
}

static void on_keyboard_backspace_clicked(GtkWidget *, gpointer user_data) {
    KeyboardDialogState *state = (KeyboardDialogState*)user_data;
    std::string text = keyboard_get_text(state);
    if (text.empty()) return;
    text.erase(text.size() - 1);
    keyboard_set_text(state, text);
}

static void on_keyboard_clear_clicked(GtkWidget *, gpointer user_data) {
    keyboard_set_text((KeyboardDialogState*)user_data, "");
}

static void on_keyboard_shift_toggled(GtkWidget *button, gpointer user_data) {
    KeyboardDialogState *state = (KeyboardDialogState*)user_data;
    state->shift = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)) != FALSE;

    for (size_t i = 0; i < state->letter_keys.size(); ++i) {
        GtkButton *key = GTK_BUTTON(state->letter_keys[i]);
        const char *current = gtk_button_get_label(key);
        if (!current || !current[0]) continue;
        char shifted[2] = { current[0], 0 };
        shifted[0] = state->shift ? g_ascii_toupper(shifted[0]) : g_ascii_tolower(shifted[0]);
        gtk_button_set_label(key, shifted);
    }
}

// One key. `character` is what gets typed; letters follow the Shift state.
static GtkWidget *keyboard_add_key(GtkWidget *row, KeyboardDialogState *state,
                                   char character, int height) {
    char label[2] = { character, 0 };
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_size_request(button, -1, height);

    KeyboardKeyData *kd = g_new0(KeyboardKeyData, 1);
    kd->state = state;
    kd->character = character;
    g_signal_connect(button, "clicked", G_CALLBACK(on_keyboard_key_clicked), kd);
    g_signal_connect(button, "destroy", G_CALLBACK(on_keyboard_key_destroy), kd);

    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    if (character >= 'a' && character <= 'z') state->letter_keys.push_back(button);
    return button;
}

static GtkWidget *keyboard_add_row(GtkWidget *keyboard_vbox, KeyboardDialogState *state,
                                   const char *keys, int height) {
    GtkWidget *row = gtk_hbox_new(TRUE, 3);
    gtk_box_pack_start(GTK_BOX(keyboard_vbox), row, FALSE, FALSE, 0);
    for (int i = 0; keys[i]; ++i) {
        keyboard_add_key(row, state, keys[i], height);
    }
    return row;
}

// -------------------------------------------------------------- the dialog

// Shows the keyboard and blocks until it is closed. `text` is the initial
// content and, when OK is pressed, receives what was typed. `max_len` caps the
// length (0 = no limit). Returns false when the dialog was cancelled, leaving
// `text` untouched.
static bool show_keyboard_dialog(GtkWindow *parent, const char *title, const char *prompt,
                                 std::string &text, size_t max_len = 0,
                                 bool start_shifted = true) {
    GdkScreen *screen = gdk_screen_get_default();
    gint width = gdk_screen_get_width(screen);
    bool is_small = (width < 1000);
    int key_height = is_small ? 42 : 64;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(KINAMP_DIALOG_TITLE,
                                                    parent,
                                                    GTK_DIALOG_MODAL,
                                                    NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), width - 40, -1);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), is_small ? 10 : 20);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *main_vbox = gtk_vbox_new(FALSE, is_small ? 5 : 10);
    gtk_container_add(GTK_CONTAINER(content_area), main_vbox);

    if (title && *title) {
        gchar *markup = g_markup_printf_escaped("<b><big>%s</big></b>", title);
        GtkWidget *title_label = gtk_label_new(markup);
        g_free(markup);
        gtk_label_set_use_markup(GTK_LABEL(title_label), TRUE);
        gtk_box_pack_start(GTK_BOX(main_vbox), title_label, FALSE, FALSE, is_small ? 2 : 5);
        gtk_box_pack_start(GTK_BOX(main_vbox), gtk_hseparator_new(), FALSE, FALSE, 0);
    }

    if (prompt && *prompt) {
        GtkWidget *prompt_label = gtk_label_new(prompt);
        gtk_misc_set_alignment(GTK_MISC(prompt_label), 0.0, 0.5);
        gtk_box_pack_start(GTK_BOX(main_vbox), prompt_label, FALSE, FALSE, 2);
    }

    KeyboardDialogState state;
    state.entry = gtk_entry_new();
    state.max_len = max_len;
    state.shift = start_shifted;

    if (max_len) gtk_entry_set_max_length(GTK_ENTRY(state.entry), (gint)max_len);
    // Not editable on purpose: tapping it must not bring up the system keyboard.
    gtk_entry_set_editable(GTK_ENTRY(state.entry), FALSE);
    gtk_widget_set_size_request(state.entry, -1, key_height);
    keyboard_set_text(&state, text);
    gtk_box_pack_start(GTK_BOX(main_vbox), state.entry, FALSE, FALSE, 5);

    GtkWidget *keyboard_vbox = gtk_vbox_new(FALSE, is_small ? 3 : 5);
    gtk_box_pack_start(GTK_BOX(main_vbox), keyboard_vbox, FALSE, FALSE, 5);

    keyboard_add_row(keyboard_vbox, &state, "1234567890", key_height);
    keyboard_add_row(keyboard_vbox, &state, "qwertyuiop", key_height);
    keyboard_add_row(keyboard_vbox, &state, "asdfghjkl", key_height);

    // Shift and Backspace share the last letter row, the way a phone keyboard
    // does, so the letters keep their own width.
    GtkWidget *letter_row = gtk_hbox_new(TRUE, 3);
    gtk_box_pack_start(GTK_BOX(keyboard_vbox), letter_row, FALSE, FALSE, 0);

    GtkWidget *shift_button = gtk_toggle_button_new_with_label("Shift");
    gtk_widget_set_size_request(shift_button, -1, key_height);
    gtk_box_pack_start(GTK_BOX(letter_row), shift_button, TRUE, TRUE, 0);

    for (const char *k = "zxcvbnm"; *k; ++k) {
        keyboard_add_key(letter_row, &state, *k, key_height);
    }

    GtkWidget *backspace_button = gtk_button_new_with_label("Del");
    gtk_widget_set_size_request(backspace_button, -1, key_height);
    g_signal_connect(backspace_button, "clicked",
                     G_CALLBACK(on_keyboard_backspace_clicked), &state);
    gtk_box_pack_start(GTK_BOX(letter_row), backspace_button, TRUE, TRUE, 0);

    // Everything a station URL may need.
    keyboard_add_row(keyboard_vbox, &state, ".:/-_~?=&%+#@,", key_height);

    GtkWidget *special_row = gtk_hbox_new(FALSE, 3);
    gtk_box_pack_start(GTK_BOX(keyboard_vbox), special_row, FALSE, FALSE, 0);

    GtkWidget *space_button = gtk_button_new_with_label("Space");
    gtk_widget_set_size_request(space_button, -1, key_height);
    g_signal_connect(space_button, "clicked", G_CALLBACK(on_keyboard_space_clicked), &state);
    gtk_box_pack_start(GTK_BOX(special_row), space_button, TRUE, TRUE, 0);

    GtkWidget *clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_size_request(clear_button, is_small ? 100 : 160, key_height);
    g_signal_connect(clear_button, "clicked", G_CALLBACK(on_keyboard_clear_clicked), &state);
    gtk_box_pack_start(GTK_BOX(special_row), clear_button, FALSE, FALSE, 0);

    // Connected last: relabelling the letters needs them to exist first.
    g_signal_connect(shift_button, "toggled", G_CALLBACK(on_keyboard_shift_toggled), &state);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(shift_button), start_shifted ? TRUE : FALSE);
    if (start_shifted) {
        // set_active() only emits when the state changes, and TRUE is the
        // default for a fresh toggle button on some themes.
        on_keyboard_shift_toggled(shift_button, &state);
    }

    gtk_box_pack_start(GTK_BOX(main_vbox), gtk_hseparator_new(), FALSE, FALSE, 5);

    // The action area owns these; packing them anywhere else first would only
    // get them reparented.
    GtkWidget *ok_button = gtk_dialog_add_button(GTK_DIALOG(dialog), "OK", GTK_RESPONSE_OK);
    GtkWidget *cancel_button = gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_widget_set_size_request(ok_button, is_small ? 120 : 200, key_height);
    gtk_widget_set_size_request(cancel_button, is_small ? 120 : 200, key_height);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    bool accepted = (response == GTK_RESPONSE_OK);
    if (accepted) text = keyboard_get_text(&state);

    gtk_widget_destroy(dialog);
    return accepted;
}

#endif // KEYBOARDDIALOG_HPP

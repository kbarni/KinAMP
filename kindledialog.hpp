#ifndef KINDLEDIALOG_HPP
#define KINDLEDIALOG_HPP

// Window handling for the Kindle window manager, shared by the dialogs.
//
// Two rules have to be followed or a window simply never appears on the
// device:
//
//  1. The title is not a caption but a hint for the window manager, telling it
//     which layer the window belongs to. A title it does not recognise means
//     the window is never mapped.
//
//  2. Only one window of the dialog layer is on screen at a time. The window
//     manager keeps them in the order they were mapped and does not raise a
//     later one above an earlier one, so a dialog opened from a dialog stays
//     invisible underneath it. The window below is therefore hidden for as
//     long as the new one is up, and shown again afterwards.
//
// Rule 2 rules out gtk_dialog_run(): it ends its loop as soon as the dialog is
// unmapped, so hiding the window underneath would close it. kindle_dialog_run()
// below is the same thing without that behaviour.

#include <gtk/gtk.h>
#include <vector>

#ifndef KINAMP_DIALOG_TITLE
#define KINAMP_DIALOG_TITLE "L:D_N:dialog_ID:com.kbarni.kinamp"
#endif

// The dialogs that are currently open, innermost last.
static std::vector<GtkWidget*> kindle_dialog_stack;

static void kindle_dialog_flush() {
    while (gtk_events_pending()) gtk_main_iteration();
}

// Shows a dialog-layer window, hiding the one it was opened from.
static void kindle_dialog_show(GtkWidget *window) {
    if (!kindle_dialog_stack.empty()) {
        gtk_widget_hide(kindle_dialog_stack.back());
        kindle_dialog_flush();
    }
    kindle_dialog_stack.push_back(window);

    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));
    kindle_dialog_flush();
}

// Closes it again and brings back the one underneath.
static void kindle_dialog_destroy(GtkWidget *window) {
    for (size_t i = kindle_dialog_stack.size(); i > 0; --i) {
        if (kindle_dialog_stack[i - 1] == window) {
            kindle_dialog_stack.erase(kindle_dialog_stack.begin() + (i - 1));
            break;
        }
    }

    gtk_widget_destroy(window);
    if (!kindle_dialog_stack.empty()) {
        GtkWidget *below = kindle_dialog_stack.back();
        gtk_widget_show(below);
        gtk_window_present(GTK_WINDOW(below));
    }
    kindle_dialog_flush();
}

struct KindleRunInfo {
    GMainLoop *loop;
    gint response;
    gboolean destroyed;
};

static void kindle_run_quit(KindleRunInfo *ri) {
    if (ri->loop && g_main_loop_is_running(ri->loop)) g_main_loop_quit(ri->loop);
}

static void on_kindle_run_response(GtkDialog *, gint response, gpointer data) {
    KindleRunInfo *ri = (KindleRunInfo*)data;
    ri->response = response;
    kindle_run_quit(ri);
}

static gboolean on_kindle_run_delete(GtkWidget *, GdkEvent *, gpointer data) {
    KindleRunInfo *ri = (KindleRunInfo*)data;
    ri->response = GTK_RESPONSE_DELETE_EVENT;
    kindle_run_quit(ri);
    return TRUE; // the caller destroys the dialog, not GTK
}

static void on_kindle_run_destroy(GtkWidget *, gpointer data) {
    KindleRunInfo *ri = (KindleRunInfo*)data;
    ri->destroyed = TRUE;
    kindle_run_quit(ri);
}

// gtk_dialog_run() minus the "quit when unmapped" rule, so the dialog survives
// being hidden while another one is open on top of it.
static gint kindle_dialog_run(GtkWidget *dialog) {
    KindleRunInfo ri;
    ri.loop = g_main_loop_new(NULL, FALSE);
    ri.response = GTK_RESPONSE_NONE;
    ri.destroyed = FALSE;

    gulong on_response = g_signal_connect(dialog, "response",
                                          G_CALLBACK(on_kindle_run_response), &ri);
    gulong on_delete = g_signal_connect(dialog, "delete-event",
                                        G_CALLBACK(on_kindle_run_delete), &ri);
    gulong on_destroy = g_signal_connect(dialog, "destroy",
                                         G_CALLBACK(on_kindle_run_destroy), &ri);

    gtk_grab_add(dialog);
    g_main_loop_run(ri.loop);

    if (!ri.destroyed) {
        gtk_grab_remove(dialog);
        g_signal_handler_disconnect(dialog, on_response);
        g_signal_handler_disconnect(dialog, on_delete);
        g_signal_handler_disconnect(dialog, on_destroy);
    }
    g_main_loop_unref(ri.loop);
    return ri.response;
}

#endif // KINDLEDIALOG_HPP

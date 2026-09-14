#define _GNU_SOURCE
#include "secure.h"
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <unistd.h>

struct Secure {
    gint refs;
    GtkWindow *parent;
    GtkWidget *box, *icons, *card, *identity, *tunnel, *notice, *connect, *rekey, *dialog, *pin_entry, *profile, *choose;
    gchar *config, *binary;
    GSubprocess *child;
    int fd, pin_fd;
    guint source, timer;
    gint64 seen, stopping;
    gboolean submitted, closing, failed, terminal_error;
    SecureStatus status;
    SecureChanged changed;
    gpointer user;
};
static GtkWidget *state_icon(const char *name, const char *stem, const char *suffix)
{
    GtkWidget *image = gtk_image_new();
    gtk_widget_set_name(image, name);
    for (guint verified = 0; verified < 2; verified++) {
        gchar *path = g_strdup_printf("/gtk-pipe/security/%s-%s%s.svg", stem,
            verified ? "verified" : "not-verified", suffix);
        GBytes *bytes = g_resources_lookup_data(path, 0, NULL);
        g_free(path);
        g_assert(bytes != NULL);
        gsize size;
        const char *data = g_bytes_get_data(bytes, &size);
        gchar *svg = g_strndup(data, size);
        gchar **parts = g_strsplit(svg, "currentColor", -1);
        gchar *colored = g_strjoinv(verified ? "#000000" : "#c62828", parts);
        GInputStream *stream = g_memory_input_stream_new_from_data(colored, -1, g_free);
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_at_scale(stream, 28, 28, TRUE, NULL, NULL);
        g_assert(pixbuf != NULL);
        g_object_set_data_full(G_OBJECT(image), verified ? "verified" : "unverified", pixbuf, g_object_unref);
        g_object_unref(stream); g_strfreev(parts); g_free(svg); g_bytes_unref(bytes);
    }
    gtk_widget_set_size_request(image, 28, 28);
    return image;
}
static void icon_state(GtkWidget *image, gboolean verified, const char *description)
{
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), g_object_get_data(G_OBJECT(image), verified ? "verified" : "unverified"));
    gtk_widget_set_tooltip_text(image, description);
    atk_object_set_name(gtk_widget_get_accessible(image), description);
}
static void notice(Secure *s, const char *message)
{
    gtk_label_set_text(GTK_LABEL(s->notice), message ? message : "");
    gtk_widget_set_visible(s->notice, message != NULL);
}
static void release(Secure *s)
{
    if (--s->refs) return;
    g_object_unref(s->icons);
    g_free(s->config); g_free(s->binary); g_free(s);
}
static void changed(Secure *s)
{
    if (s->changed) s->changed(s->user, &s->status);
}
static void erase(void *p, gsize n)
{
    volatile unsigned char *b = p;
    while (n--) *b++ = 0;
}
static void close_pin(Secure *s)
{
    if (s->pin_fd >= 0) { close(s->pin_fd); s->pin_fd = -1; }
}
static void stop_child(Secure *s)
{
    close_pin(s);
    if (s->child && !s->stopping) {
        s->stopping = g_get_monotonic_time();
        g_subprocess_send_signal(s->child, SIGTERM);
    }
    if (s->dialog) gtk_dialog_response(GTK_DIALOG(s->dialog), GTK_RESPONSE_CANCEL);
}
static void failure(Secure *s, const char *message)
{
    s->failed = TRUE;
    s->status.state = SEC_ERROR;
    if (!s->terminal_error) s->status.reason = 0;
    icon_state(s->card, FALSE, "Card: unknown / unavailable");
    icon_state(s->tunnel, FALSE, message);
    icon_state(s->identity, FALSE, "Identity: locked / unavailable");
    notice(s, message);
    gtk_widget_set_sensitive(s->connect, FALSE);
    gtk_widget_set_sensitive(s->rekey, FALSE);
    changed(s);
    stop_child(s);
}
static const char *reason_text(guint reason)
{
    static const char *text[] = { "Ready", "Insert the configured card",
        "Card service unavailable — check pcscd and the reader",
        "User PIN blocked — administrator recovery required",
        "Incorrect PIN — no automatic retry", "Card login failed",
        "Card identity or signing mechanism could not be verified",
        "Card/reader health lost — reconnect with a fresh PIN",
        "Invalid profile or public-key pin", "Backend startup or shutdown failure", "Another secure window already uses these ports",
        "PIN retries running low — check your PIN before continuing",
        "Final PIN attempt — an incorrect PIN will block the card" };
    return reason < G_N_ELEMENTS(text) ? text[reason] : "Backend failure";
}
static void render(Secure *s)
{
    guint state = s->status.state;
    const char *card = state == SEC_WAIT_CARD ? reason_text(s->status.reason) :
                       state == SEC_ERROR || state == SEC_STOPPED ? "Unknown / unavailable" : "Configured card detected";
    gchar *line = g_strdup_printf("Card: %s", card);
    icon_state(s->card, state >= SEC_PIN_REQUIRED && state <= SEC_ESTABLISHED, line); g_free(line);
    icon_state(s->identity, state >= SEC_WAIT_PEER && state <= SEC_ESTABLISHED,
        state == SEC_PIN_REQUIRED && s->status.reason ? reason_text(s->status.reason) :
        state >= SEC_WAIT_PEER && state <= SEC_ESTABLISHED ? "Identity: unlocked and verified" :
        state == SEC_AUTHENTICATING ? "Identity: checking PIN and signing key…" : "Identity: locked");
    const char *tunnel = state == SEC_ESTABLISHED ? "Tunnel: secure connection established" :
        state == SEC_WAIT_PEER ? "Tunnel: waiting for peer / reconnecting" :
        state == SEC_CONNECTING ? "Tunnel: authenticating peer / refreshing session" :
        state == SEC_ERROR ? reason_text(s->status.reason) : "Tunnel: disconnected";
    icon_state(s->tunnel, state == SEC_ESTABLISHED, tunnel);
    const char *summary = state == SEC_WAIT_CARD ? "Insert card / Disconnected" :
        state == SEC_PIN_REQUIRED ? "Card inserted / Disconnected — activate to enter PIN" :
        state == SEC_AUTHENTICATING ? "Card inserted / Verifying identity…" :
        state == SEC_WAIT_PEER ? "Identity verified / Waiting for peer…" :
        state == SEC_CONNECTING ? "Identity verified / Connecting…" :
        state == SEC_ESTABLISHED ? "Card inserted / Connected" : "Inactive / Disconnected";
    notice(s, s->status.reason && (state == SEC_PIN_REQUIRED || state == SEC_ERROR ||
        (state == SEC_WAIT_CARD && s->status.reason != 1)) ? reason_text(s->status.reason) : summary);
    gtk_button_set_label(GTK_BUTTON(s->connect), state == SEC_PIN_REQUIRED ? "Activate" :
        state == SEC_AUTHENTICATING || state == SEC_WAIT_CARD || state == SEC_CONNECTING ? "Cancel" : "Deactivate");
    gtk_widget_set_tooltip_text(s->connect, state == SEC_PIN_REQUIRED ? "Enter your PIN to unlock the identity and connect" : "Disconnect and lock the identity");
    gtk_widget_set_sensitive(s->connect, !s->stopping);
    gtk_widget_set_sensitive(s->rekey, state == SEC_ESTABLISHED && !s->stopping);
    if (s->dialog && state != SEC_PIN_REQUIRED)
        gtk_dialog_response(GTK_DIALOG(s->dialog), GTK_RESPONSE_CANCEL);
    changed(s);
}
static gboolean receive_status(gint fd, GIOCondition condition, gpointer data)
{
    Secure *s = data;
    (void)condition;
    for (guint i = 0; i < 16; i++) {
        char b[512];
        ssize_t n = recv(fd, b, sizeof(b), MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return G_SOURCE_CONTINUE;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            s->source = 0;
            if (!s->stopping) failure(s, "Tunnel: backend connection closed");
            return G_SOURCE_REMOVE;
        }
        SecureStatus next;
        if (!secure_status_parse(b, (gsize)n, &next) || next.generation < s->status.generation) {
            s->source = 0; failure(s, "Tunnel: incompatible or invalid backend status");
            return G_SOURCE_REMOVE;
        }
        s->seen = g_get_monotonic_time();
        if (next.state == SEC_ERROR && !s->terminal_error && (!s->stopping || s->failed)) {
            s->terminal_error = TRUE; s->status = next;
            failure(s, reason_text(next.reason));
            continue;
        }
        if (s->stopping) continue;
        if (s->submitted && next.state == SEC_PIN_REQUIRED) next.state = SEC_AUTHENTICATING;
        s->status = next;
        render(s);
        if (next.state == SEC_STOPPED) stop_child(s);
    }
    return G_SOURCE_CONTINUE;
}
static void child_exited(GObject *object, GAsyncResult *result, gpointer data)
{
    Secure *s = data;
    GError *error = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(object), result, &error);
    g_clear_error(&error);
    if (s->source) { g_source_remove(s->source); s->source = 0; }
    /* Child completion and socket readiness can arrive in either order.
     * Preserve a queued terminal error, but never apply an established state
     * after the child has exited. */
    g_clear_object(&s->child);
    if (s->fd >= 0) {
        for (guint i=0; i<32; i++) {
            char b[512]; ssize_t n=recv(s->fd,b,sizeof(b),MSG_DONTWAIT|MSG_TRUNC);
            if (n<=0) break;
            SecureStatus status;
            if (!s->closing && !s->terminal_error && (!s->stopping || s->failed) &&
                secure_status_parse(b,(gsize)n,&status) && status.state==SEC_ERROR) {
                s->terminal_error=TRUE; s->status=status; failure(s,reason_text(status.reason));
            }
        }
        close(s->fd); s->fd = -1;
    }
    close_pin(s);
    if (!s->closing) {
        if (!s->failed) {
            s->status.state = SEC_STOPPED;
            icon_state(s->tunnel, FALSE, "Tunnel: disconnected");
            notice(s, "Inactive / Disconnected — activate to check card");
        }
        icon_state(s->card, FALSE, "Card: not monitored — activate to check card");
        icon_state(s->identity, FALSE, "Identity: locked");
        gtk_button_set_label(GTK_BUTTON(s->connect), "Activate");
        gtk_widget_set_tooltip_text(s->connect, "Check card and reconnect");
        gtk_widget_set_sensitive(s->connect, TRUE);
        gtk_widget_set_sensitive(s->choose, TRUE);
        gtk_widget_set_sensitive(s->rekey, FALSE);
        changed(s);
    }
    s->stopping = 0;
    release(s);
}
static gboolean tick(gpointer data)
{
    Secure *s = data;
    gint64 now = g_get_monotonic_time();
    if (!s->child) return G_SOURCE_CONTINUE;
    if (s->stopping) {
        if (now - s->stopping > 2 * G_USEC_PER_SEC) g_subprocess_force_exit(s->child);
    } else if (now - s->seen > 4 * G_USEC_PER_SEC) {
        failure(s, "Tunnel: backend status timed out — connection closed");
    } else {
        ssize_t n = send(s->fd, "PING", 4, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            failure(s, "Tunnel: backend unavailable");
    }
    return G_SOURCE_CONTINUE;
}
void secure_start(Secure *s)
{
    if (s->child || s->closing) return;
    int pair[2], pin[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair)) {
        failure(s, "Cannot create private backend connection"); return;
    }
    if (pipe2(pin, O_CLOEXEC | O_NONBLOCK)) {
        close(pair[0]); close(pair[1]); failure(s, "Cannot create private PIN pipe"); return;
    }
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_take_fd(launcher, pair[1], 3);
    g_subprocess_launcher_take_fd(launcher, pin[0], 4);
    GError *error = NULL;
    s->child = g_subprocess_launcher_spawn(launcher, &error, s->binary,
        "--config", s->config, "--supervise-fd", "3", "--pin-fd", "4", NULL);
    g_object_unref(launcher);
    if (!s->child) {
        close(pair[0]); close(pin[1]);
        failure(s, error->message); g_clear_error(&error);
        gtk_button_set_label(GTK_BUTTON(s->connect), "Activate");
        gtk_widget_set_sensitive(s->connect, TRUE); return;
    }
    gtk_widget_set_sensitive(s->choose, FALSE);
    s->fd = pair[0]; s->pin_fd = pin[1];
    s->seen = g_get_monotonic_time(); s->stopping = 0;
    s->submitted = FALSE; s->failed = FALSE; s->terminal_error = FALSE;
    s->status = (SecureStatus){ .state=SEC_WAIT_CARD, .mtu=1400 };
    s->source = g_unix_fd_add(s->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, receive_status, s);
    s->refs++;
    g_subprocess_wait_async(s->child, NULL, child_exited, s);
    render(s);
    tick(s);
}
static void pin_response(GtkDialog *dialog, gint answer, gpointer data)
{
    Secure *s = data;
    GtkWidget *entry = s->pin_entry;
    s->dialog = NULL; s->pin_entry = NULL;
    const char *value = gtk_entry_get_text(GTK_ENTRY(entry));
    gsize n = strlen(value);
    if (answer == GTK_RESPONSE_OK && n && n <= 126 && !strchr(value, '\n') && !strchr(value, '\r') &&
        s->child && !s->stopping && s->status.state == SEC_PIN_REQUIRED) {
        char pin[128] = {0}; memcpy(pin, value, n); pin[n++] = '\n';
        /* <= PIPE_BUF, nonblocking: one complete write or failure, never retry a PIN. */
        ssize_t written = write(s->pin_fd, pin, n);
        erase(pin, sizeof(pin)); close_pin(s); s->submitted = TRUE;
        if (written != (ssize_t)n) failure(s, "PIN could not be delivered — reconnect to try again");
        else { s->status.state = SEC_AUTHENTICATING; render(s); }
    }
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void connect_clicked(GtkButton *button, gpointer data)
{
    Secure *s = data;
    (void)button;
    if (!s->child) { secure_start(s); return; }
    if (s->status.state != SEC_PIN_REQUIRED || s->submitted) {
        s->status.state = SEC_STOPPED; render(s); stop_child(s); return;
    }
    if (s->dialog) { gtk_window_present(GTK_WINDOW(s->dialog)); return; }
    s->dialog = gtk_dialog_new_with_buttons("Unlock SmartCard-HSM", s->parent,
        GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL, "Connect", GTK_RESPONSE_OK, NULL);
    GtkEntryBuffer *buffer = secure_pin_buffer_new();
    GtkWidget *entry = gtk_entry_new_with_buffer(buffer); s->pin_entry = entry;
    g_object_unref(buffer);
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_input_hints(GTK_ENTRY(entry), GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_NO_EMOJI);
    gtk_entry_set_max_length(GTK_ENTRY(entry), 126);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(s->dialog), GTK_RESPONSE_OK);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(s->dialog));
    gtk_box_pack_start(GTK_BOX(area), gtk_label_new(s->status.reason ? reason_text(s->status.reason) : "User PIN (one login attempt; never saved)"), FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 8);
    g_signal_connect(s->dialog, "response", G_CALLBACK(pin_response), s);
    gtk_widget_show_all(s->dialog);
    gtk_widget_grab_focus(entry);
}
static void rekey_clicked(GtkButton *button, gpointer data)
{
    Secure *s = data; (void)button;
    if (s->child && s->status.state == SEC_ESTABLISHED)
        if (send(s->fd, "REKEY", 5, MSG_DONTWAIT | MSG_NOSIGNAL) != 5)
            failure(s, "Could not request a new secure session");
}
static void profile_label(Secure *s)
{
    GKeyFile *key = g_key_file_new();
    gchar *peer = NULL, *identity = NULL;
    if (g_key_file_load_from_file(key, s->config, G_KEY_FILE_NONE, NULL)) {
        peer = g_key_file_get_string(key, "peer", "name", NULL);
        identity = g_key_file_get_string(key, "identity", "name", NULL);
    }
    gchar *base = g_path_get_basename(s->config);
    gchar *title = g_strdup_printf("Secure profile: %s\n%s → %s", base,
        identity ? identity : "Local identity", peer ? peer : "Configured peer");
    gtk_label_set_text(GTK_LABEL(s->profile), title);
    gtk_widget_set_tooltip_text(s->profile, s->config);
    g_free(title); g_free(base); g_free(peer); g_free(identity); g_key_file_unref(key);
}
static void choose_response(GtkDialog *dialog, gint answer, gpointer data)
{
    Secure *s = data;
    s->dialog = NULL;
    if (answer == GTK_RESPONSE_ACCEPT && !s->child) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) { g_free(s->config); s->config = path; profile_label(s); }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void choose_clicked(GtkButton *button, gpointer data)
{
    Secure *s = data; (void)button;
    if (s->child || s->dialog) return;
    s->dialog = gtk_file_chooser_dialog_new("Select provisioned hsmproxy profile", s->parent,
        GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL, "Select", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(s->dialog), s->config);
    g_signal_connect(s->dialog, "response", G_CALLBACK(choose_response), s);
    gtk_widget_show(s->dialog);
}
Secure *secure_new(GtkWindow *parent, const char *config, const char *binary,
                   SecureChanged callback, gpointer user)
{
    /* GTK handles the PIN too; refuse secure mode if process dump protection fails. */
    struct rlimit limit = {0, 0};
    if (setrlimit(RLIMIT_CORE, &limit) || prctl(PR_SET_DUMPABLE, 0)) return NULL;
    signal(SIGPIPE, SIG_IGN);
    Secure *s = g_new0(Secure, 1); s->refs = 1; s->fd = s->pin_fd = -1;
    s->parent = parent; s->config = g_canonicalize_filename(config, NULL);
    s->binary = g_strdup(binary); s->changed = callback; s->user = user;
    s->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    s->icons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    g_object_ref_sink(s->icons);
    gtk_box_pack_start(GTK_BOX(s->box), panel, FALSE, FALSE, 0);
    s->profile = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(s->profile), 0);
    gtk_label_set_ellipsize(GTK_LABEL(s->profile), PANGO_ELLIPSIZE_END);
    profile_label(s);
    s->card = state_icon("secure-card", "smartcard", "-icon");
    s->tunnel = state_icon("secure-tunnel", "tunnel-lock", "-icon");
    s->identity = state_icon("secure-identity", "identity", "");
    GtkWidget *images[] = {s->card, s->tunnel, s->identity};
    for (guint i=0; i<G_N_ELEMENTS(images); i++) {
        gtk_box_pack_start(GTK_BOX(s->icons), images[i], FALSE, FALSE, 0);
    }
    icon_state(s->card, FALSE, "Card: not monitored");
    icon_state(s->identity, FALSE, "Identity: locked");
    icon_state(s->tunnel, FALSE, "Tunnel: disconnected");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
    s->connect = gtk_button_new_with_label("Activate");
    s->rekey = gtk_button_new_with_label("Reload");
    s->choose = gtk_button_new_with_label("Profile");
    gtk_widget_set_tooltip_text(s->connect, "Check card and connect");
    gtk_widget_set_tooltip_text(s->rekey, "Refresh the secure session (rekey)");
    gtk_widget_set_tooltip_text(s->choose, "Select a profile after deactivating");
    g_signal_connect(s->choose, "clicked", G_CALLBACK(choose_clicked), s);
    gtk_widget_set_sensitive(s->rekey, FALSE);
    g_signal_connect(s->connect, "clicked", G_CALLBACK(connect_clicked), s);
    g_signal_connect(s->rekey, "clicked", G_CALLBACK(rekey_clicked), s);
    gtk_box_pack_start(GTK_BOX(row), s->connect, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), s->rekey, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), s->choose, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), s->profile, TRUE, TRUE, 0);
    s->notice = gtk_label_new("Inactive / Disconnected — activate to check card");
    gtk_widget_set_name(s->notice, "secure-state");
    gtk_label_set_xalign(GTK_LABEL(s->notice), 0);
    gtk_label_set_line_wrap(GTK_LABEL(s->notice), TRUE);
    gtk_box_pack_start(GTK_BOX(s->box), s->notice, FALSE, FALSE, 0);
    s->timer = g_timeout_add(500, tick, s);
    return s;
}
GtkWidget *secure_widget(Secure *s) { return s->box; }
GtkWidget *secure_status_widget(Secure *s) { return s->icons; }
void secure_free(Secure *s)
{
    if (!s) return;
    s->closing = TRUE; s->changed = NULL;
    if (s->dialog) gtk_dialog_response(GTK_DIALOG(s->dialog), GTK_RESPONSE_CANCEL);
    if (s->source) { g_source_remove(s->source); s->source = 0; }
    if (s->timer) { g_source_remove(s->timer); s->timer = 0; }
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    close_pin(s);
    if (s->child) g_subprocess_force_exit(s->child);
    release(s); /* outstanding child wait owns the remaining reference */
}

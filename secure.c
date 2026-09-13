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
    GtkWidget *box, *card, *identity, *tunnel, *details, *connect, *rekey, *dialog, *pin_entry, *profile, *choose;
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
static void release(Secure *s)
{
    if (--s->refs) return;
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
    gtk_label_set_text(GTK_LABEL(s->tunnel), message);
    gtk_label_set_text(GTK_LABEL(s->identity), "Identity: locked / unavailable");
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
    gtk_label_set_text(GTK_LABEL(s->card), line); g_free(line);
    gtk_label_set_text(GTK_LABEL(s->identity),
        state == SEC_PIN_REQUIRED && s->status.reason ? reason_text(s->status.reason) :
        state >= SEC_WAIT_PEER && state <= SEC_ESTABLISHED ? "Identity: unlocked and verified" :
        state == SEC_AUTHENTICATING ? "Identity: checking PIN and signing key…" : "Identity: locked");
    const char *tunnel = state == SEC_ESTABLISHED ? "Tunnel: secure connection established" :
        state == SEC_WAIT_PEER ? "Tunnel: waiting for peer / reconnecting" :
        state == SEC_CONNECTING ? "Tunnel: authenticating peer / refreshing session" :
        state == SEC_ERROR ? reason_text(s->status.reason) : "Tunnel: disconnected";
    gtk_label_set_text(GTK_LABEL(s->tunnel), tunnel);
    gtk_button_set_label(GTK_BUTTON(s->connect), state == SEC_PIN_REQUIRED ? "Connect…" : "Disconnect & lock");
    gtk_widget_set_sensitive(s->connect, !s->stopping);
    gtk_widget_set_sensitive(s->rekey, state == SEC_ESTABLISHED && !s->stopping);
    line = g_strdup_printf("Session generation: %" G_GUINT64_FORMAT "   Payload limit: %u bytes\n"
        "Video / audio / text TX: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "\n"
        "Video / audio / text RX: %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "\n"
        "Rejected: %" G_GUINT64_FORMAT "   Oversize: %" G_GUINT64_FORMAT "   MTU drops: %" G_GUINT64_FORMAT,
        s->status.generation, s->status.mtu,
        s->status.tx[0], s->status.tx[1], s->status.tx[2],
        s->status.rx[0], s->status.rx[1], s->status.rx[2],
        s->status.rejected, s->status.oversize, s->status.mtu_drops);
    gtk_label_set_text(GTK_LABEL(s->details), line); g_free(line);
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
            gtk_label_set_text(GTK_LABEL(s->tunnel), "Tunnel: disconnected");
        }
        gtk_label_set_text(GTK_LABEL(s->card), "Card: not monitored — select Check card to reconnect");
        gtk_label_set_text(GTK_LABEL(s->identity), "Identity: locked");
        gtk_button_set_label(GTK_BUTTON(s->connect), "Check card / reconnect");
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
        gtk_button_set_label(GTK_BUTTON(s->connect), "Retry backend");
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
    s->profile = gtk_label_new(NULL);
    profile_label(s);
    gtk_box_pack_start(GTK_BOX(s->box), s->profile, FALSE, FALSE, 0);
    s->card = gtk_label_new("Card: checking…");
    s->identity = gtk_label_new("Identity: locked");
    s->tunnel = gtk_label_new("Tunnel: disconnected");
    GtkWidget *labels[] = {s->card, s->identity, s->tunnel};
    for (guint i=0; i<G_N_ELEMENTS(labels); i++) {
        gtk_label_set_xalign(GTK_LABEL(labels[i]), 0);
        gtk_label_set_line_wrap(GTK_LABEL(labels[i]), TRUE);
        gtk_box_pack_start(GTK_BOX(s->box), labels[i], FALSE, FALSE, 0);
    }
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    s->connect = gtk_button_new_with_label("Check card");
    s->rekey = gtk_button_new_with_label("Refresh session");
    s->choose = gtk_button_new_with_label("Change profile…");
    g_signal_connect(s->choose, "clicked", G_CALLBACK(choose_clicked), s);
    gtk_widget_set_sensitive(s->rekey, FALSE);
    g_signal_connect(s->connect, "clicked", G_CALLBACK(connect_clicked), s);
    g_signal_connect(s->rekey, "clicked", G_CALLBACK(rekey_clicked), s);
    gtk_box_pack_start(GTK_BOX(row), s->connect, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), s->rekey, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), s->choose, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(s->box), row, FALSE, FALSE, 0);
    GtkWidget *expander = gtk_expander_new("Connection details");
    s->details = gtk_label_new("No session");
    gtk_label_set_selectable(GTK_LABEL(s->details), TRUE);
    gtk_label_set_xalign(GTK_LABEL(s->details), 0);
    gtk_container_add(GTK_CONTAINER(expander), s->details);
    gtk_box_pack_start(GTK_BOX(s->box), expander, FALSE, FALSE, 0);
    s->timer = g_timeout_add(500, tick, s);
    return s;
}
GtkWidget *secure_widget(Secure *s) { return s->box; }
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

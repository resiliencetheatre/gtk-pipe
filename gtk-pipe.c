/* gtk-pipe - a small two-way RTP audio/video link over UDP. */
#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_VIDEO_PORT 5000
#define DEFAULT_AUDIO_PORT 5002
#define DEFAULT_TEXT_PORT 5004
#define MAX_TEXT_BYTES 1024
#define TEXT_PREFIX "GTKPIPE/1 TEXT "
#define PING_MESSAGE "GTKPIPE/1 PING"
#define PONG_MESSAGE "GTKPIPE/1 PONG"
#define HEARTBEAT_SECONDS 2
#define REACHABLE_TIMEOUT_SECONDS 6

typedef struct {
    GtkWidget *window;
    GtkWidget *peer_entry;
    GtkWidget *peer_indicator;
    GtkWidget *button;
    GtkWidget *status;
    GtkWidget *video_box;
    GtkWidget *video_widget;
    GtkWidget *text_view;
    GtkWidget *text_entry;
    GtkWidget *send_button;
    GstElement *pipeline;
    guint bus_watch;
    GSocket *text_socket;
    GSource *text_source;
    guint heartbeat_timer;
    gchar *text_peer;
    gint64 last_peer_seen;
    guint video_port;
    guint audio_port;
    guint text_port;
} App;

static void close_text_channel(App *app);

static void set_peer_reachable(App *app, gboolean reachable)
{
    const char *color = reachable ? "#2ecc71" : "#808080";
    const char *tip = reachable ? "GTK Pipe peer reachable"
                                : "Waiting for GTK Pipe peer response";
    gchar *markup = g_strdup_printf("<span foreground=\"%s\" size=\"large\">●</span>",
                                    color);
    gtk_label_set_markup(GTK_LABEL(app->peer_indicator), markup);
    gtk_widget_set_tooltip_text(app->peer_indicator, tip);
    g_free(markup);
}

static void set_status(App *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status), text);
}

static void append_message(App *app, const char *who, const char *message)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->text_view));
    GtkTextIter end;
    GDateTime *now = g_date_time_new_now_local();
    gchar *time = g_date_time_format(now, "%H:%M:%S");
    gchar *line = g_strdup_printf("[%s] %s: %s\n", time, who, message);

    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, line, -1);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->text_view), &end,
                                 0.0, FALSE, 0.0, 1.0);
    g_free(line);
    g_free(time);
    g_date_time_unref(now);
}

static gboolean receive_text(GSocket *socket, GIOCondition condition,
                             gpointer data)
{
    App *app = data;
    gchar buffer[MAX_TEXT_BYTES + sizeof(TEXT_PREFIX) + 1];
    GError *error = NULL;
    gssize length;

    if (condition & (G_IO_ERR | G_IO_HUP))
        return G_SOURCE_CONTINUE;

    length = g_socket_receive(socket, buffer, MAX_TEXT_BYTES, NULL, &error);
    if (length < 0) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            g_printerr("Text receive error: %s\n", error->message);
        g_clear_error(&error);
        return G_SOURCE_CONTINUE;
    }
    if (length == 0)
        return G_SOURCE_CONTINUE;
    buffer[length] = '\0';

    if (!g_strcmp0(buffer, PING_MESSAGE)) {
        g_socket_send(socket, PONG_MESSAGE, strlen(PONG_MESSAGE), NULL, NULL);
        app->last_peer_seen = g_get_monotonic_time();
        set_peer_reachable(app, TRUE);
        return G_SOURCE_CONTINUE;
    }
    if (!g_strcmp0(buffer, PONG_MESSAGE)) {
        app->last_peer_seen = g_get_monotonic_time();
        set_peer_reachable(app, TRUE);
        return G_SOURCE_CONTINUE;
    }

    const gchar *message = g_str_has_prefix(buffer, TEXT_PREFIX)
                         ? buffer + strlen(TEXT_PREFIX) : buffer;
    gsize message_length = length - (message - buffer);
    if (message_length == 0 || message_length > MAX_TEXT_BYTES ||
        memchr(message, '\0', message_length) ||
        !g_utf8_validate(message, message_length, NULL)) {
        g_printerr("Ignored a non-UTF-8 text datagram\n");
        return G_SOURCE_CONTINUE;
    }
    app->last_peer_seen = g_get_monotonic_time();
    set_peer_reachable(app, TRUE);
    append_message(app, "Peer", message);
    return G_SOURCE_CONTINUE;
}

static gboolean start_text_channel(App *app, const char *peer, GError **error)
{
    GInetAddress *address = g_inet_address_new_from_string(peer);
    GSocketAddress *local;
    GSocketAddress *remote;
    GSocketFamily family = g_inet_address_get_family(address);
    GInetAddress *any = g_inet_address_new_any(family);

    app->text_socket = g_socket_new(family, G_SOCKET_TYPE_DATAGRAM,
                                    G_SOCKET_PROTOCOL_UDP, error);
    local = g_inet_socket_address_new(any, app->text_port);
    remote = g_inet_socket_address_new(address, app->text_port);
    g_object_unref(any);
    g_object_unref(address);

    if (!app->text_socket ||
        !g_socket_bind(app->text_socket, local, TRUE, error) ||
        !g_socket_connect(app->text_socket, remote, NULL, error)) {
        g_clear_object(&app->text_socket);
        g_object_unref(local);
        g_object_unref(remote);
        return FALSE;
    }
    g_object_unref(local);
    g_object_unref(remote);

    app->text_source = g_socket_create_source(app->text_socket, G_IO_IN, NULL);
    g_source_set_callback(app->text_source, G_SOURCE_FUNC(receive_text), app, NULL);
    g_source_attach(app->text_source, NULL);
    app->text_peer = g_strdup(peer);
    gtk_widget_set_sensitive(app->text_entry, TRUE);
    gtk_widget_set_sensitive(app->send_button, TRUE);
    return TRUE;
}

static void close_text_channel(App *app)
{
    if (app->text_source) {
        g_source_destroy(app->text_source);
        g_source_unref(app->text_source);
        app->text_source = NULL;
    }
    g_clear_object(&app->text_socket);
    g_clear_pointer(&app->text_peer, g_free);
    app->last_peer_seen = 0;
    set_peer_reachable(app, FALSE);
    gtk_widget_set_sensitive(app->text_entry, FALSE);
    gtk_widget_set_sensitive(app->send_button, FALSE);
}

static gboolean refresh_text_channel(App *app)
{
    const char *peer = gtk_entry_get_text(GTK_ENTRY(app->peer_entry));
    GInetAddress *address;
    GError *error = NULL;

    if (app->text_socket && !g_strcmp0(peer, app->text_peer))
        return TRUE;

    close_text_channel(app);
    address = g_inet_address_new_from_string(peer);
    if (!address)
        return FALSE;
    g_object_unref(address);

    if (!start_text_channel(app, peer, &error)) {
        g_printerr("Could not open text channel: %s\n", error->message);
        g_clear_error(&error);
        return FALSE;
    }
    return TRUE;
}

static gboolean heartbeat_text_channel(gpointer data)
{
    App *app = data;
    gint64 now = g_get_monotonic_time();

    if (!refresh_text_channel(app))
        return G_SOURCE_CONTINUE;
    if (app->last_peer_seen == 0 ||
        now - app->last_peer_seen > REACHABLE_TIMEOUT_SECONDS * G_USEC_PER_SEC)
        set_peer_reachable(app, FALSE);
    if (g_socket_send(app->text_socket, PING_MESSAGE, strlen(PING_MESSAGE),
                      NULL, NULL) < 0)
        set_peer_reachable(app, FALSE);
    return G_SOURCE_CONTINUE;
}

static void stop_stream(App *app)
{
    if (app->bus_watch) {
        g_source_remove(app->bus_watch);
        app->bus_watch = 0;
    }
    if (app->pipeline) {
        gst_element_set_state(app->pipeline, GST_STATE_NULL);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
    }
    if (app->video_widget) {
        gtk_container_remove(GTK_CONTAINER(app->video_box), app->video_widget);
        app->video_widget = NULL;
    }
    gtk_button_set_label(GTK_BUTTON(app->button), "Start stream");
    gtk_widget_set_sensitive(app->peer_entry, TRUE);
    set_status(app, app->text_socket ? "Media stopped; text channel active"
                                     : "Media stopped");
}

static gboolean bus_message(GstBus *bus, GstMessage *message, gpointer data)
{
    App *app = data;
    (void)bus;

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        g_printerr("GStreamer error from %s: %s\n",
                   GST_OBJECT_NAME(message->src), error->message);
        if (debug)
            g_printerr("Debug details: %s\n", debug);
        gchar *message_text = g_strdup(error->message);
        g_clear_error(&error);
        g_free(debug);
        stop_stream(app);
        set_status(app, message_text);
        g_free(message_text);
    }
    return G_SOURCE_CONTINUE;
}

static gchar *make_pipeline(const char *peer, guint video_port, guint audio_port)
{
    return g_strdup_printf(
        "autovideosrc ! videoconvert ! videoscale ! "
        "video/x-raw,width=640,height=480,framerate=15/1 ! queue ! "
        "vp8enc deadline=1 cpu-used=8 target-bitrate=600000 keyframe-max-dist=30 "
        "! rtpvp8pay pt=96 ! udpsink host=\"%s\" port=%u sync=false async=false "
        "autoaudiosrc ! audioconvert ! audioresample ! "
        "audio/x-raw,rate=48000,channels=1 ! queue ! "
        "opusenc bitrate=32000 inband-fec=true ! rtpopuspay pt=97 ! "
        "udpsink host=\"%s\" port=%u sync=false async=false "
        "udpsrc port=%u caps=\"application/x-rtp,media=video,clock-rate=90000,"
        "encoding-name=VP8,payload=96\" ! rtpjitterbuffer latency=120 "
        "drop-on-latency=true ! rtpvp8depay ! vp8dec ! videoconvert ! "
        "gtksink name=remote_video sync=false "
        "udpsrc port=%u caps=\"application/x-rtp,media=audio,clock-rate=48000,"
        "encoding-name=OPUS,payload=97\" ! rtpjitterbuffer latency=120 "
        "drop-on-latency=true ! rtpopusdepay ! opusdec plc=true ! "
        "audioconvert ! audioresample ! autoaudiosink sync=false",
        peer, video_port, peer, audio_port, video_port, audio_port);
}

static gboolean start_stream(App *app)
{
    const char *peer = gtk_entry_get_text(GTK_ENTRY(app->peer_entry));
    GInetAddress *address = g_inet_address_new_from_string(peer);
    GError *error = NULL;
    gchar *description;
    GstElement *sink;
    GstBus *bus;

    if (!address) {
        set_status(app, "Enter a numeric IPv4 or IPv6 peer address");
        return FALSE;
    }
    g_object_unref(address);

    description = make_pipeline(peer, app->video_port, app->audio_port);
    app->pipeline = gst_parse_launch(description, &error);
    g_free(description);
    if (!app->pipeline) {
        set_status(app, error ? error->message : "Could not create pipeline");
        g_clear_error(&error);
        return FALSE;
    }
    if (error) {
        gchar *message_text = g_strdup(error->message);
        g_clear_error(&error);
        stop_stream(app);
        set_status(app, message_text);
        g_free(message_text);
        return FALSE;
    }

    sink = gst_bin_get_by_name(GST_BIN(app->pipeline), "remote_video");
    if (!sink) {
        set_status(app, "GStreamer gtksink is unavailable");
        stop_stream(app);
        return FALSE;
    }
    g_object_get(sink, "widget", &app->video_widget, NULL);
    gst_object_unref(sink);
    gtk_box_pack_start(GTK_BOX(app->video_box), app->video_widget, TRUE, TRUE, 0);
    gtk_widget_show(app->video_widget);
    g_object_unref(app->video_widget); /* The container now owns the widget. */

    bus = gst_element_get_bus(app->pipeline);
    app->bus_watch = gst_bus_add_watch(bus, bus_message, app);
    gst_object_unref(bus);
    if (gst_element_set_state(app->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        stop_stream(app);
        set_status(app, "Could not start the media devices");
        return FALSE;
    }

    gtk_button_set_label(GTK_BUTTON(app->button), "Stop stream");
    gtk_widget_set_sensitive(app->peer_entry, FALSE);
    gtk_widget_grab_focus(app->text_entry);
    gchar *status = g_strdup_printf(
        "Streaming with %s (video %u, audio %u, text %u UDP)",
        peer, app->video_port, app->audio_port, app->text_port);
    set_status(app, status);
    g_free(status);
    return TRUE;
}

static void send_text(GtkWidget *widget, gpointer data)
{
    App *app = data;
    const gchar *message = gtk_entry_get_text(GTK_ENTRY(app->text_entry));
    gsize length = strlen(message);
    GError *error = NULL;
    gssize sent;
    gchar *datagram;
    gsize datagram_length;
    (void)widget;

    if (length == 0)
        return;
    if (length > MAX_TEXT_BYTES) {
        set_status(app, "Text messages are limited to 1024 UTF-8 bytes");
        return;
    }
    if (!refresh_text_channel(app)) {
        set_status(app, "Enter a valid peer address for text messaging");
        return;
    }
    datagram = g_strconcat(TEXT_PREFIX, message, NULL);
    datagram_length = strlen(datagram);
    sent = g_socket_send(app->text_socket, datagram, datagram_length, NULL, &error);
    g_free(datagram);
    if (sent != (gssize)datagram_length) {
        set_status(app, error ? error->message : "Text message was not sent");
        g_clear_error(&error);
        return;
    }
    append_message(app, "Me", message);
    gtk_entry_set_text(GTK_ENTRY(app->text_entry), "");
}

static void button_clicked(GtkButton *button, gpointer data)
{
    App *app = data;
    (void)button;
    if (app->pipeline)
        stop_stream(app);
    else
        start_stream(app);
}

static void window_destroy(GtkWidget *widget, gpointer data)
{
    App *app = data;
    (void)widget;
    stop_stream(app);
    if (app->heartbeat_timer) {
        g_source_remove(app->heartbeat_timer);
        app->heartbeat_timer = 0;
    }
    close_text_channel(app);
    gtk_main_quit();
}

static gboolean parse_port(const char *text, guint *port)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (!*text || !end || *end || value < 1 || value > 65535)
        return FALSE;
    *port = (guint)value;
    return TRUE;
}

static void build_ui(App *app, const char *peer)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new("Peer address:");
    GtkWidget *text_scroll;
    GtkWidget *message_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "GTK Pipe");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 700, 560);
    gtk_container_set_border_width(GTK_CONTAINER(app->window), 12);
    g_signal_connect(app->window, "destroy", G_CALLBACK(window_destroy), app);

    app->peer_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app->peer_entry), peer);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->peer_entry), "192.0.2.10");
    app->button = gtk_button_new_with_label("Start stream");
    app->peer_indicator = gtk_label_new(NULL);
    set_peer_reachable(app, FALSE);
    g_signal_connect(app->button, "clicked", G_CALLBACK(button_clicked), app);
    gtk_box_pack_start(GTK_BOX(controls), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), app->peer_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(controls), app->peer_indicator, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), app->button, FALSE, FALSE, 0);

    app->video_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(app->video_box, 640, 360);
    app->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->text_view), GTK_WRAP_WORD_CHAR);
    text_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(text_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(text_scroll, -1, 110);
    gtk_container_add(GTK_CONTAINER(text_scroll), app->text_view);

    app->text_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->text_entry), "Short message");
    gtk_entry_set_max_length(GTK_ENTRY(app->text_entry), MAX_TEXT_BYTES);
    app->send_button = gtk_button_new_with_label("Send");
    gtk_widget_set_sensitive(app->text_entry, FALSE);
    gtk_widget_set_sensitive(app->send_button, FALSE);
    g_signal_connect(app->send_button, "clicked", G_CALLBACK(send_text), app);
    g_signal_connect(app->text_entry, "activate", G_CALLBACK(send_text), app);
    gtk_box_pack_start(GTK_BOX(message_controls), app->text_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(message_controls), app->send_button, FALSE, FALSE, 0);
    app->status = gtk_label_new("Media stopped");
    gtk_label_set_xalign(GTK_LABEL(app->status), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(app->status), PANGO_ELLIPSIZE_END);

    gtk_box_pack_start(GTK_BOX(root), controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), app->video_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), text_scroll, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), message_controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), app->status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), root);
    gtk_widget_show_all(app->window);
    heartbeat_text_channel(app);
    app->heartbeat_timer = g_timeout_add_seconds(HEARTBEAT_SECONDS,
                                                 heartbeat_text_channel, app);
}

int main(int argc, char **argv)
{
    App app = { .video_port = DEFAULT_VIDEO_PORT,
                .audio_port = DEFAULT_AUDIO_PORT,
                .text_port = DEFAULT_TEXT_PORT };
    const char *peer = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        if (!g_strcmp0(argv[i], "--peer") && i + 1 < argc)
            peer = argv[++i];
        else if (!g_strcmp0(argv[i], "--video-port") && i + 1 < argc) {
            if (!parse_port(argv[++i], &app.video_port)) {
                g_printerr("Invalid video port\n"); return EXIT_FAILURE;
            }
        } else if (!g_strcmp0(argv[i], "--audio-port") && i + 1 < argc) {
            if (!parse_port(argv[++i], &app.audio_port)) {
                g_printerr("Invalid audio port\n"); return EXIT_FAILURE;
            }
        } else if (!g_strcmp0(argv[i], "--text-port") && i + 1 < argc) {
            if (!parse_port(argv[++i], &app.text_port)) {
                g_printerr("Invalid text port\n"); return EXIT_FAILURE;
            }
        } else if (!g_strcmp0(argv[i], "--help")) {
            g_print("Usage: %s [--peer ADDRESS] [--video-port PORT] "
                    "[--audio-port PORT] [--text-port PORT]\n", argv[0]);
            return EXIT_SUCCESS;
        } else {
            g_printerr("Unknown or incomplete option: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }
    if (app.video_port == app.audio_port || app.video_port == app.text_port ||
        app.audio_port == app.text_port) {
        g_printerr("Audio, video, and text ports must be different\n");
        return EXIT_FAILURE;
    }

    gst_init(&argc, &argv);
    gtk_init(&argc, &argv);
    build_ui(&app, peer);
    gtk_main();
    return EXIT_SUCCESS;
}

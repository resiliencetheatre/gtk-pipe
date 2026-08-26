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
    gint width;
    gint height;
    gint fps_n;
    gint fps_d;
} VideoMode;

typedef struct {
    GtkWidget *window;
    GtkWidget *peer_entry;
    GtkWidget *peer_indicator;
    GtkWidget *button;
    GtkWidget *status;
    GtkWidget *video_box;
    GtkWidget *video_widget;
    GtkWidget *local_video_widget;
    GtkWidget *local_preview_frame;
    GtkWidget *video_settings_label;
    GtkWidget *text_view;
    GtkWidget *text_entry;
    GtkWidget *send_button;
    GstElement *pipeline;
    GstElement *capture_caps;
    GstElement *opus_encoder;
    guint bus_watch;
    GSocket *text_socket;
    GSource *text_source;
    guint heartbeat_timer;
    gchar *text_peer;
    gint64 last_peer_seen;
    guint video_port;
    guint audio_port;
    guint text_port;
    GArray *video_modes;
    gchar *video_device;
    guint quality_index;
    gboolean enable_controls;
} App;

#define MAX_QUALITY_OPTIONS 4
#define MIN_OPUS_BITRATE 8000
#define MAX_OPUS_BITRATE 64000

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
        g_clear_pointer(&app->capture_caps, gst_object_unref);
        g_clear_pointer(&app->opus_encoder, gst_object_unref);
        gst_object_unref(app->pipeline);
        app->pipeline = NULL;
    }
    if (app->video_widget) {
        gtk_container_remove(GTK_CONTAINER(app->video_box), app->video_widget);
        app->video_widget = NULL;
    }
    if (app->local_preview_frame) {
        gtk_container_remove(GTK_CONTAINER(app->video_box),
                             app->local_preview_frame);
        app->local_preview_frame = NULL;
        app->local_video_widget = NULL;
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

static gint compare_video_modes(gconstpointer a, gconstpointer b)
{
    const VideoMode *ma = a;
    const VideoMode *mb = b;
    gint64 area_a = (gint64)ma->width * ma->height;
    gint64 area_b = (gint64)mb->width * mb->height;
    gint64 rate_a = (gint64)ma->fps_n * mb->fps_d;
    gint64 rate_b = (gint64)mb->fps_n * ma->fps_d;

    if (area_a != area_b)
        return area_a < area_b ? -1 : 1;
    if (rate_a != rate_b)
        return rate_a < rate_b ? -1 : 1;
    return 0;
}

static void add_video_mode(GArray *modes, gint width, gint height,
                           gint fps_n, gint fps_d)
{
    VideoMode mode = { width, height, fps_n, fps_d };

    if (width <= 0 || height <= 0 || fps_n <= 0 || fps_d <= 0)
        return;
    for (guint i = 0; i < modes->len; i++) {
        VideoMode *old = &g_array_index(modes, VideoMode, i);
        if (old->width == width && old->height == height &&
            (gint64)old->fps_n * fps_d == (gint64)fps_n * old->fps_d)
            return;
    }
    g_array_append_val(modes, mode);
}

static void add_framerates(GArray *modes, gint width, gint height,
                           const GValue *value)
{
    if (GST_VALUE_HOLDS_FRACTION(value)) {
        add_video_mode(modes, width, height,
                       gst_value_get_fraction_numerator(value),
                       gst_value_get_fraction_denominator(value));
    } else if (GST_VALUE_HOLDS_LIST(value)) {
        for (guint i = 0; i < gst_value_list_get_size(value); i++)
            add_framerates(modes, width, height,
                           gst_value_list_get_value(value, i));
    } else if (GST_VALUE_HOLDS_FRACTION_RANGE(value)) {
        add_framerates(modes, width, height,
                       gst_value_get_fraction_range_min(value));
        add_framerates(modes, width, height,
                       gst_value_get_fraction_range_max(value));
    }
}

static gboolean discover_video_modes(App *app)
{
    GstDeviceMonitor *monitor = gst_device_monitor_new();
    GstCaps *filter = gst_caps_from_string("video/x-raw");
    GList *devices;
    GArray *all = g_array_new(FALSE, FALSE, sizeof(VideoMode));

    gst_device_monitor_add_filter(monitor, "Video/Source", filter);
    gst_caps_unref(filter);
    if (!gst_device_monitor_start(monitor)) {
        gst_object_unref(monitor);
        g_array_unref(all);
        return FALSE;
    }
    devices = gst_device_monitor_get_devices(monitor);
    for (GList *item = devices; item && !app->video_device; item = item->next) {
        GstDevice *device = item->data;
        GstStructure *properties = gst_device_get_properties(device);
        GstCaps *caps = gst_device_get_caps(device);
        const gchar *path = properties
                          ? gst_structure_get_string(properties, "device.path")
                          : NULL;

        if (!path || !g_str_has_prefix(path, "/dev/video")) {
            if (properties)
                gst_structure_free(properties);
            gst_caps_unref(caps);
            continue;
        }
        for (guint i = 0; i < gst_caps_get_size(caps); i++) {
            const GstStructure *s = gst_caps_get_structure(caps, i);
            gint width, height;
            const GValue *framerate;

            if (g_strcmp0(gst_structure_get_name(s), "video/x-raw") ||
                !gst_structure_get_int(s, "width", &width) ||
                !gst_structure_get_int(s, "height", &height))
                continue;
            framerate = gst_structure_get_value(s, "framerate");
            if (framerate)
                add_framerates(all, width, height, framerate);
        }
        if (all->len)
            app->video_device = g_strdup(path);
        if (properties)
            gst_structure_free(properties);
        gst_caps_unref(caps);
    }
    g_list_free_full(devices, gst_object_unref);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);

    if (!all->len) {
        g_array_unref(all);
        return FALSE;
    }
    g_array_sort(all, compare_video_modes);
    app->video_modes = g_array_new(FALSE, FALSE, sizeof(VideoMode));
    if (all->len <= MAX_QUALITY_OPTIONS) {
        g_array_append_vals(app->video_modes, all->data, all->len);
    } else {
        guint picks[] = { 0, 1, all->len - 2, all->len - 1 };
        for (guint i = 0; i < G_N_ELEMENTS(picks); i++) {
            VideoMode mode = g_array_index(all, VideoMode, picks[i]);
            g_array_append_val(app->video_modes, mode);
        }
    }
    g_array_unref(all);
    app->quality_index = (app->video_modes->len - 1) / 2;
    return TRUE;
}

static guint opus_bitrate(const App *app)
{
    if (app->video_modes->len < 2)
        return MIN_OPUS_BITRATE;
    return MIN_OPUS_BITRATE +
        (MAX_OPUS_BITRATE - MIN_OPUS_BITRATE) * app->quality_index /
        (app->video_modes->len - 1);
}

static gchar *make_pipeline(const App *app, const char *peer)
{
    const VideoMode *mode = &g_array_index(app->video_modes, VideoMode,
                                           app->quality_index);
    gchar *escaped_device = g_strescape(app->video_device, NULL);
    gchar *pipeline = g_strdup_printf(
        "v4l2src device=\"%s\" ! capsfilter name=capture_caps "
        "caps=\"video/x-raw,width=%d,height=%d,framerate=%d/%d\" ! "
        "videoconvert ! tee name=camera_tee "
        "camera_tee. ! queue ! "
        "vp8enc deadline=1 cpu-used=8 target-bitrate=600000 keyframe-max-dist=30 "
        "! rtpvp8pay pt=96 ! udpsink host=\"%s\" port=%u sync=false async=false "
        "camera_tee. ! queue leaky=downstream max-size-buffers=1 ! videoscale ! "
        "video/x-raw,width=160,height=120 ! videoconvert ! "
        "gtksink name=local_preview sync=false qos=false "
        "autoaudiosrc ! audioconvert ! audioresample ! "
        "audio/x-raw,rate=48000,channels=1 ! queue ! "
        "opusenc name=opus_encoder bitrate=%u inband-fec=true ! rtpopuspay pt=97 ! "
        "udpsink host=\"%s\" port=%u sync=false async=false "
        "udpsrc port=%u caps=\"application/x-rtp,media=video,clock-rate=90000,"
        "encoding-name=VP8,payload=96\" ! rtpjitterbuffer latency=120 "
        "drop-on-latency=true ! rtpvp8depay ! vp8dec ! videoconvert ! "
        "gtksink name=remote_video sync=false "
        "udpsrc port=%u caps=\"application/x-rtp,media=audio,clock-rate=48000,"
        "encoding-name=OPUS,payload=97\" ! rtpjitterbuffer latency=120 "
        "drop-on-latency=true ! rtpopusdepay ! opusdec plc=true ! "
        "audioconvert ! audioresample ! autoaudiosink sync=false",
        escaped_device, mode->width, mode->height, mode->fps_n, mode->fps_d,
        peer, app->video_port, opus_bitrate(app), peer, app->audio_port,
        app->video_port, app->audio_port);
    g_free(escaped_device);
    return pipeline;
}

static void update_video_settings(App *app)
{
    const VideoMode *mode = &g_array_index(app->video_modes, VideoMode,
                                           app->quality_index);
    gchar *label = g_strdup_printf(
        "Resolution: %d×%d    FPS: %.2f",
        mode->width, mode->height, (gdouble)mode->fps_n / mode->fps_d);
    if (app->video_settings_label)
        gtk_label_set_text(GTK_LABEL(app->video_settings_label), label);
    g_free(label);

    if (app->capture_caps) {
        GstCaps *caps = gst_caps_new_simple("video/x-raw",
            "width", G_TYPE_INT, mode->width,
            "height", G_TYPE_INT, mode->height,
            "framerate", GST_TYPE_FRACTION, mode->fps_n, mode->fps_d,
            NULL);
        g_object_set(app->capture_caps, "caps", caps, NULL);
        gst_caps_unref(caps);
    }
    if (app->opus_encoder)
        g_object_set(app->opus_encoder, "bitrate", opus_bitrate(app), NULL);
}

static void quality_changed(GtkRange *range, gpointer data)
{
    App *app = data;
    app->quality_index = (guint)gtk_range_get_value(range);
    update_video_settings(app);
}

static GtkWidget *make_vertical_control(const char *title, guint maximum,
                                        guint selected, GCallback callback,
                                        App *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *label = gtk_label_new(title);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
                                                0, maximum, 1);
    gtk_range_set_inverted(GTK_RANGE(scale), TRUE);
    gtk_range_set_value(GTK_RANGE(scale), selected);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(scale), 0);
    gtk_widget_set_vexpand(scale, TRUE);
    gtk_widget_set_tooltip_text(scale, title);
    g_signal_connect(scale, "value-changed", callback, app);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scale, TRUE, TRUE, 0);
    return box;
}

static gboolean start_stream(App *app)
{
    const char *peer = gtk_entry_get_text(GTK_ENTRY(app->peer_entry));
    GInetAddress *address = g_inet_address_new_from_string(peer);
    GError *error = NULL;
    gchar *description;
    GstElement *sink;
    GstElement *local_sink;
    GstBus *bus;

    if (!address) {
        set_status(app, "Enter a numeric IPv4 or IPv6 peer address");
        return FALSE;
    }
    g_object_unref(address);

    description = make_pipeline(app, peer);
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

    app->capture_caps = gst_bin_get_by_name(GST_BIN(app->pipeline),
                                             "capture_caps");
    app->opus_encoder = gst_bin_get_by_name(GST_BIN(app->pipeline),
                                             "opus_encoder");

    sink = gst_bin_get_by_name(GST_BIN(app->pipeline), "remote_video");
    if (!sink) {
        set_status(app, "GStreamer gtksink is unavailable");
        stop_stream(app);
        return FALSE;
    }
    g_object_get(sink, "widget", &app->video_widget, NULL);
    gst_object_unref(sink);
    gtk_container_add(GTK_CONTAINER(app->video_box), app->video_widget);
    gtk_widget_show(app->video_widget);
    g_object_unref(app->video_widget); /* The container now owns the widget. */

    local_sink = gst_bin_get_by_name(GST_BIN(app->pipeline), "local_preview");
    if (!local_sink) {
        stop_stream(app);
        set_status(app, "GStreamer local preview sink is unavailable");
        return FALSE;
    }
    g_object_get(local_sink, "widget", &app->local_video_widget, NULL);
    gst_object_unref(local_sink);
    app->local_preview_frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(app->local_preview_frame), GTK_SHADOW_IN);
    gtk_widget_set_halign(app->local_preview_frame, GTK_ALIGN_END);
    gtk_widget_set_valign(app->local_preview_frame, GTK_ALIGN_START);
    gtk_widget_set_margin_top(app->local_preview_frame, 8);
    gtk_widget_set_margin_end(app->local_preview_frame, 8);
    gtk_container_add(GTK_CONTAINER(app->local_preview_frame),
                      app->local_video_widget);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->video_box),
                            app->local_preview_frame);
    gtk_widget_show_all(app->local_preview_frame);
    g_object_unref(app->local_video_widget);

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
    GtkWidget *video_area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *video_column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *quality_control;

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "GTK Pipe");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 640, 560);
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

    app->video_box = gtk_overlay_new();
    gtk_widget_set_hexpand(app->video_box, TRUE);
    gtk_widget_set_vexpand(app->video_box, TRUE);
    gtk_box_pack_start(GTK_BOX(video_column), app->video_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(video_area), video_column, TRUE, TRUE, 0);
    if (app->enable_controls) {
        PangoAttrList *small_attrs = pango_attr_list_new();
        app->video_settings_label = gtk_label_new(NULL);
        pango_attr_list_insert(small_attrs,
                               pango_attr_scale_new(PANGO_SCALE_SMALL));
        gtk_label_set_attributes(GTK_LABEL(app->video_settings_label), small_attrs);
        pango_attr_list_unref(small_attrs);
        gtk_label_set_xalign(GTK_LABEL(app->video_settings_label), 0.5);
        quality_control = make_vertical_control(
            "quality", app->video_modes->len - 1, app->quality_index,
            G_CALLBACK(quality_changed), app);
        gtk_box_pack_start(GTK_BOX(video_column), app->video_settings_label,
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(video_area), quality_control,
                           FALSE, FALSE, 0);
        update_video_settings(app);
    }
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
    gtk_box_pack_start(GTK_BOX(root), video_area, TRUE, TRUE, 0);
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
        } else if (!g_strcmp0(argv[i], "--enable-controls")) {
            app.enable_controls = TRUE;
        } else if (!g_strcmp0(argv[i], "--help")) {
            g_print("Usage: %s [--peer ADDRESS] [--video-port PORT] "
                    "[--audio-port PORT] [--text-port PORT] "
                    "[--enable-controls]\n", argv[0]);
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
    if (!discover_video_modes(&app)) {
        g_printerr("No V4L2 camera with supported raw-video modes was found\n");
        return EXIT_FAILURE;
    }
    gtk_init(&argc, &argv);
    build_ui(&app, peer);
    gtk_main();
    g_array_unref(app.video_modes);
    g_free(app.video_device);
    return EXIT_SUCCESS;
}

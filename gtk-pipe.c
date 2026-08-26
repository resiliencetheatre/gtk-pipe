/* gtk-pipe - a small two-way RTP audio/video link over UDP. */
#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gio/gio.h>
#include <stdlib.h>

#define DEFAULT_VIDEO_PORT 5000
#define DEFAULT_AUDIO_PORT 5002

typedef struct {
    GtkWidget *window;
    GtkWidget *peer_entry;
    GtkWidget *button;
    GtkWidget *status;
    GtkWidget *video_box;
    GtkWidget *video_widget;
    GstElement *pipeline;
    guint bus_watch;
    guint video_port;
    guint audio_port;
} App;

static void set_status(App *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status), text);
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
    set_status(app, "Stopped");
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
    gchar *status = g_strdup_printf("Streaming with %s (video UDP %u, audio UDP %u)",
                                    peer, app->video_port, app->audio_port);
    set_status(app, status);
    g_free(status);
    return TRUE;
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

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "GTK Pipe");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 700, 560);
    gtk_container_set_border_width(GTK_CONTAINER(app->window), 12);
    g_signal_connect(app->window, "destroy", G_CALLBACK(window_destroy), app);

    app->peer_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app->peer_entry), peer);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->peer_entry), "192.0.2.10");
    app->button = gtk_button_new_with_label("Start stream");
    g_signal_connect(app->button, "clicked", G_CALLBACK(button_clicked), app);
    gtk_box_pack_start(GTK_BOX(controls), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), app->peer_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(controls), app->button, FALSE, FALSE, 0);

    app->video_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(app->video_box, 640, 480);
    app->status = gtk_label_new("Stopped");
    gtk_label_set_xalign(GTK_LABEL(app->status), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(app->status), PANGO_ELLIPSIZE_END);

    gtk_box_pack_start(GTK_BOX(root), controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), app->video_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), app->status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), root);
    gtk_widget_show_all(app->window);
}

int main(int argc, char **argv)
{
    App app = { .video_port = DEFAULT_VIDEO_PORT,
                .audio_port = DEFAULT_AUDIO_PORT };
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
        } else if (!g_strcmp0(argv[i], "--help")) {
            g_print("Usage: %s [--peer ADDRESS] [--video-port PORT] [--audio-port PORT]\n", argv[0]);
            return EXIT_SUCCESS;
        } else {
            g_printerr("Unknown or incomplete option: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }
    if (app.video_port == app.audio_port) {
        g_printerr("Audio and video ports must be different\n");
        return EXIT_FAILURE;
    }

    gst_init(&argc, &argv);
    gtk_init(&argc, &argv);
    build_ui(&app, peer);
    gtk_main();
    return EXIT_SUCCESS;
}

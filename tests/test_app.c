/* Exercise the actual media/text gating without a real camera or card. */
#define main gtk_pipe_application_main
#include "../gtk-pipe.c"
#undef main
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
static gboolean test(gpointer ignored) {
    (void)ignored;
    App app={.rtp_mtu=1400,.site_name=g_strdup("test"),
        .bind_address=g_strdup("127.0.0.1"),.video_source=g_strdup("videotestsrc"),
        .video_modes=g_array_new(FALSE,FALSE,sizeof(VideoMode))};
    VideoMode mode={320,240,30,1}; g_array_append_val(app.video_modes,mode);
    int sockets[3]; guint ports[3];
    for(guint i=0;i<3;i++) {
        sockets[i]=socket(AF_INET,SOCK_DGRAM,0); g_assert_cmpint(sockets[i],>=,0);
        struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};
        g_assert_cmpint(bind(sockets[i],(struct sockaddr *)&addr,sizeof(addr)),==,0);
        socklen_t len=sizeof(addr); g_assert_cmpint(getsockname(sockets[i],(struct sockaddr *)&addr,&len),==,0);
        ports[i]=ntohs(addr.sin_port);
    }
    for(guint i=0;i<3;i++) close(sockets[i]);
    app.video_port=ports[0]; app.audio_port=ports[1]; app.text_port=ports[2];
    build_ui(&app,"127.0.0.2");
    /* Standalone text starts independently of any backend. */
    g_assert_nonnull(app.text_socket);
    gchar *pipeline=make_pipeline(&app,"127.0.0.2");
    g_assert_nonnull(strstr(pipeline,"mtu=1400")); g_free(pipeline);
    app.secure_mode=TRUE;
    SecureStatus status={.state=SEC_PIN_REQUIRED,.ports={ports[0],ports[1],ports[2]},.mtu=1100};
    secure_changed(&app,&status);
    g_assert_null(app.text_socket); g_assert_false(app.secure_ready);
    g_assert_false(gtk_widget_get_sensitive(app.button));
    g_assert_false(start_stream(&app)); g_assert_null(app.pipeline);
    status.state=SEC_ESTABLISHED; secure_changed(&app,&status);
    g_assert_nonnull(app.text_socket); g_assert_true(app.secure_ready);
    g_assert_false(gtk_widget_get_sensitive(app.peer_entry));
    g_assert_cmpuint(app.rtp_mtu,==,1100);
    pipeline=make_pipeline(&app,"127.0.0.2");
    g_assert_nonnull(strstr(pipeline,"mtu=1100")); g_free(pipeline);
    app.secure_payload=64;
    gtk_entry_set_text(GTK_ENTRY(app.text_entry),"This message exceeds the negotiated payload size after text protocol framing.");
    send_text(NULL,&app);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(app.status)),"payload limit"));
    /* A synthetic live pipeline proves actual capture teardown on gate loss. */
    app.pipeline=gst_parse_launch("videotestsrc is-live=true ! fakesink",NULL);
    g_assert_nonnull(app.pipeline);
    gst_element_set_state(app.pipeline,GST_STATE_PLAYING);
    status.state=SEC_ERROR; secure_changed(&app,&status);
    g_assert_null(app.pipeline); g_assert_null(app.text_socket);
    status.state=SEC_ESTABLISHED; secure_changed(&app,&status);
    g_assert_null(app.pipeline); /* reconnect never implicitly starts capture */
    gtk_widget_destroy(app.window);
    g_array_unref(app.video_modes); g_free(app.video_source); g_free(app.site_name); g_free(app.bind_address);
    g_print("app: standalone text, secure gates, fixed peer, negotiated video MTU, text size and capture teardown passed\n");
    return G_SOURCE_REMOVE;
}
int main(int argc,char **argv) {
    gtk_init(&argc,&argv); gst_init(&argc,&argv);
    g_idle_add(test,NULL); gtk_main(); return 0;
}

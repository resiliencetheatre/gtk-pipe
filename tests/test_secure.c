#include "secure.h"
#include <string.h>
static SecureStatus latest;
static guint updates;
static void changed(gpointer user,const SecureStatus *status) {
    (void)user; latest=*status; updates++;
}
static void spin(guint ms) {
    gint64 end=g_get_monotonic_time()+(gint64)ms*1000;
    while(g_get_monotonic_time()<end) {
        while(g_main_context_iteration(NULL,FALSE)) {}
        g_usleep(1000);
    }
}
static void wait_state(guint state) {
    for(guint i=0;i<600 && latest.state!=state;i++) spin(10);
    g_assert_cmpuint(latest.state,==,state);
}
static GtkWidget *find(GtkWidget *widget,GType type,const char *label) {
    if(G_TYPE_CHECK_INSTANCE_TYPE(widget,type) &&
       (!label || (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)),label)))) return widget;
    if(GTK_IS_CONTAINER(widget)) {
        GList *children=gtk_container_get_children(GTK_CONTAINER(widget));
        GtkWidget *found=NULL;
        for(GList *c=children;c && !found;c=c->next) found=find(c->data,type,label);
        g_list_free(children); return found;
    }
    return NULL;
}
static GtkWidget *dialog(void) {
    GList *windows=gtk_window_list_toplevels(); GtkWidget *out=NULL;
    for(GList *w=windows;w;w=w->next)
        if(GTK_IS_DIALOG(w->data) && gtk_widget_get_visible(w->data)) out=w->data;
    g_list_free(windows); return out;
}
static void check_icons(GtkWidget *widget, gboolean card, gboolean tunnel, gboolean identity) {
    if(GTK_IS_IMAGE(widget)) {
        const char *name=gtk_widget_get_name(widget);
        gboolean verified=!strcmp(name,"secure-card") ? card : !strcmp(name,"secure-tunnel") ? tunnel : identity;
        GdkPixbuf *pixbuf=gtk_image_get_pixbuf(GTK_IMAGE(widget));
        g_assert_nonnull(pixbuf);
        g_assert_cmpint(gdk_pixbuf_get_width(pixbuf),<=,28);
        g_assert_cmpint(gdk_pixbuf_get_height(pixbuf),<=,28);
        g_assert_true(gdk_pixbuf_get_has_alpha(pixbuf));
        g_assert_cmpint(gdk_pixbuf_get_pixels(pixbuf)[3],==,0);
        GdkRGBA *background=NULL;
        gtk_style_context_get(gtk_widget_get_style_context(widget),GTK_STATE_FLAG_NORMAL,
            "background-color",&background,NULL);
        g_assert_nonnull(background);
        g_assert_cmpfloat(background->alpha,==,0);
        gdk_rgba_free(background);
        guint painted=0;
        int channels=gdk_pixbuf_get_n_channels(pixbuf);
        for(int y=0;y<gdk_pixbuf_get_height(pixbuf);y++) {
            const guchar *row=gdk_pixbuf_get_pixels(pixbuf)+y*gdk_pixbuf_get_rowstride(pixbuf);
            for(int x=0;x<gdk_pixbuf_get_width(pixbuf);x++) {
                const guchar *p=row+x*channels;
                if(channels==4 && p[3]<200) continue;
                if(verified) { g_assert_cmpint(p[0],<,10); g_assert_cmpint(p[1],<,10); }
                else { g_assert_cmpint(p[0],>,150); g_assert_cmpint(p[1],<,80); }
                painted++;
            }
        }
        g_assert_cmpuint(painted,>,20);
    }
    g_assert_false(GTK_IS_EXPANDER(widget));
    if(GTK_IS_CONTAINER(widget)) {
        GList *children=gtk_container_get_children(GTK_CONTAINER(widget));
        for(GList *c=children;c;c=c->next) check_icons(c->data,card,tunnel,identity);
        g_list_free(children);
    }
}
static void snapshot(GtkWidget *window, const char *state) {
    const char *directory=g_getenv("UI_SNAPSHOT_DIR");
    if(!directory) return;
    spin(100);
    GtkRequisition minimum,natural;
    gtk_widget_get_preferred_size(window,&minimum,&natural);
    GtkAllocation allocation={0,0,MAX(616,natural.width),natural.height};
    gtk_widget_size_allocate(window,&allocation);
    cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        gtk_widget_get_allocated_width(window),gtk_widget_get_allocated_height(window));
    cairo_t *cr=cairo_create(surface);
    gtk_widget_draw(window,cr);
    gchar *path=g_strdup_printf("%s/security-%s.png",directory,state);
    g_assert_cmpint(cairo_surface_write_to_png(surface,path),==,CAIRO_STATUS_SUCCESS);
    g_free(path); cairo_destroy(cr); cairo_surface_destroy(surface);
}
static void pin(Secure *s,gboolean submit) {
    GtkWidget *button=find(secure_widget(s),GTK_TYPE_BUTTON,"Activate"); g_assert_nonnull(button);
    gtk_button_clicked(GTK_BUTTON(button)); spin(50);
    GtkWidget *d=dialog(); g_assert_nonnull(d);
    GtkWidget *entry=find(d,GTK_TYPE_ENTRY,NULL); g_assert_nonnull(entry);
    g_assert_false(gtk_entry_get_visibility(GTK_ENTRY(entry)));
    gtk_entry_set_text(GTK_ENTRY(entry),"1234");
    gtk_dialog_response(GTK_DIALOG(d),submit?GTK_RESPONSE_OK:GTK_RESPONSE_CANCEL);
}
int main(int argc,char **argv) {
    gtk_init(&argc,&argv);
    g_assert_cmpint(argc,==,2);
    GtkEntryBuffer *buffer=secure_pin_buffer_new();
    gtk_entry_buffer_set_text(buffer,"12é34",-1);
    g_assert_cmpuint(gtk_entry_buffer_get_bytes(buffer),==,6);
    gtk_entry_buffer_delete_text(buffer,2,1);
    g_assert_cmpstr(gtk_entry_buffer_get_text(buffer),==,"1234");
    gtk_entry_buffer_set_text(buffer,"",0);
    const char *cleared=gtk_entry_buffer_get_text(buffer);
    for(guint i=0;i<6;i++) g_assert_cmpint(cleared[i],==,0);
    char long_pin[201]; memset(long_pin,'1',200); long_pin[200]=0;
    gtk_entry_buffer_set_text(buffer,long_pin,-1);
    g_assert_cmpuint(gtk_entry_buffer_get_bytes(buffer),==,126);
    g_object_unref(buffer);
    const char *modes[]={"normal","badpin","stale","malformed","immediate"};
    for(guint i=0;i<G_N_ELEMENTS(modes);i++) {
        GtkWidget *window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_default_size(GTK_WINDOW(window),616,-1);
        latest=(SecureStatus){.state=SEC_STOPPED}; updates=0;
        Secure *s=secure_new(GTK_WINDOW(window),modes[i],argv[1],changed,NULL); g_assert_nonnull(s);
        GtkWidget *layout=gtk_box_new(GTK_ORIENTATION_VERTICAL,12);
        gtk_container_add(GTK_CONTAINER(window),layout);
        gtk_box_pack_start(GTK_BOX(layout),secure_widget(s),FALSE,FALSE,0);
        GtkWidget *bottom=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,16);
        gtk_box_pack_start(GTK_BOX(bottom),secure_status_widget(s),FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(bottom),gtk_label_new("Media stopped"),TRUE,TRUE,0);
        gtk_box_pack_end(GTK_BOX(layout),bottom,FALSE,FALSE,0);
        gtk_widget_show_all(window);
        secure_start(s);
        if(i<2) {
            wait_state(SEC_PIN_REQUIRED);
            check_icons(secure_status_widget(s),TRUE,FALSE,FALSE);
            if(i==0) {
                int minimum,natural;
                gtk_widget_get_preferred_height(secure_widget(s),&minimum,&natural);
                g_assert_cmpint(natural,<=,72);
                snapshot(window,"locked");
            }
            pin(s,FALSE); spin(150); g_assert_cmpuint(latest.state,==,SEC_PIN_REQUIRED);
            pin(s,TRUE);
            if(i==0) {
                wait_state(SEC_ESTABLISHED);
                check_icons(secure_status_widget(s),TRUE,TRUE,TRUE);
                snapshot(window,"verified");
                GtkWidget *rekey=find(secure_widget(s),GTK_TYPE_BUTTON,"Reload");
                gtk_button_clicked(GTK_BUTTON(rekey)); spin(300);
                g_assert_cmpuint(latest.generation,==,2);
                GtkWidget *stop=find(secure_widget(s),GTK_TYPE_BUTTON,"Deactivate");
                gtk_button_clicked(GTK_BUTTON(stop)); wait_state(SEC_STOPPED); spin(200);
                check_icons(secure_status_widget(s),FALSE,FALSE,FALSE);
                secure_start(s); wait_state(SEC_PIN_REQUIRED);
                /* Destroy while a PIN dialog is open: no stale callbacks. */
                GtkWidget *connect=find(secure_widget(s),GTK_TYPE_BUTTON,"Activate");
                gtk_button_clicked(GTK_BUTTON(connect)); g_assert_nonnull(dialog());
            } else { wait_state(SEC_ERROR); spin(200); g_assert_cmpuint(latest.reason,==,4); check_icons(secure_status_widget(s),FALSE,FALSE,FALSE); }
        } else { wait_state(SEC_ERROR); check_icons(secure_status_widget(s),FALSE,FALSE,FALSE); if(i==4) { spin(200); g_assert_cmpuint(latest.reason,==,8); } }
        secure_free(s); gtk_widget_destroy(window); spin(200);
        g_assert_cmpuint(updates,>,0);
    }
    g_print("secure GUI: PIN cancel/submit, buffer erasure, rekey, disconnect/restart, bad PIN, stale/malformed status and dialog teardown passed\n");
}

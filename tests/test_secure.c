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
static void pin(Secure *s,gboolean submit) {
    GtkWidget *button=find(secure_widget(s),GTK_TYPE_BUTTON,"Connect…"); g_assert_nonnull(button);
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
        latest=(SecureStatus){.state=SEC_STOPPED}; updates=0;
        Secure *s=secure_new(GTK_WINDOW(window),modes[i],argv[1],changed,NULL); g_assert_nonnull(s);
        gtk_container_add(GTK_CONTAINER(window),secure_widget(s)); gtk_widget_show_all(window);
        secure_start(s);
        if(i<2) {
            wait_state(SEC_PIN_REQUIRED);
            pin(s,FALSE); spin(150); g_assert_cmpuint(latest.state,==,SEC_PIN_REQUIRED);
            pin(s,TRUE);
            if(i==0) {
                wait_state(SEC_ESTABLISHED);
                GtkWidget *rekey=find(secure_widget(s),GTK_TYPE_BUTTON,"Refresh session");
                gtk_button_clicked(GTK_BUTTON(rekey)); spin(300);
                g_assert_cmpuint(latest.generation,==,2);
                GtkWidget *stop=find(secure_widget(s),GTK_TYPE_BUTTON,"Disconnect & lock");
                gtk_button_clicked(GTK_BUTTON(stop)); wait_state(SEC_STOPPED); spin(200);
                secure_start(s); wait_state(SEC_PIN_REQUIRED);
                /* Destroy while a PIN dialog is open: no stale callbacks. */
                GtkWidget *connect=find(secure_widget(s),GTK_TYPE_BUTTON,"Connect…");
                gtk_button_clicked(GTK_BUTTON(connect)); g_assert_nonnull(dialog());
            } else { wait_state(SEC_ERROR); spin(200); g_assert_cmpuint(latest.reason,==,4); }
        } else { wait_state(SEC_ERROR); if(i==4) { spin(200); g_assert_cmpuint(latest.reason,==,8); } }
        secure_free(s); gtk_widget_destroy(window); spin(200);
        g_assert_cmpuint(updates,>,0);
    }
    g_print("secure GUI: PIN cancel/submit, buffer erasure, rekey, disconnect/restart, bad PIN, stale/malformed status and dialog teardown passed\n");
}

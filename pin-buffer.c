#include "secure.h"
#include <string.h>
/* Fixed storage avoids reallocating live PIN text. Deleted bytes and the final
 * buffer are explicitly erased. This does not protect against a compromised OS. */
typedef struct { GtkEntryBuffer parent; gchar text[128]; gsize bytes; } PinBuffer;
typedef struct { GtkEntryBufferClass parent; } PinBufferClass;
G_DEFINE_TYPE(PinBuffer, pin_buffer, GTK_TYPE_ENTRY_BUFFER)
static void wipe(void *data, gsize bytes) {
    volatile unsigned char *p=data; while(bytes--) *p++=0;
}
static const gchar *pin_text(GtkEntryBuffer *buffer,gsize *bytes) {
    PinBuffer *p=(PinBuffer *)buffer; if(bytes) *bytes=p->bytes; return p->text;
}
static guint pin_length(GtkEntryBuffer *buffer) {
    return (guint)g_utf8_strlen(((PinBuffer *)buffer)->text,-1);
}
static guint pin_insert(GtkEntryBuffer *buffer,guint position,const gchar *text,guint chars) {
    PinBuffer *p=(PinBuffer *)buffer;
    guint accepted=0; gsize size=0;
    while(accepted<chars) {
        const gchar *next=g_utf8_next_char(text+size);
        gsize bytes=(gsize)(next-text);
        if(p->bytes+bytes>126) break;
        size=bytes; accepted++;
    }
    if(!size) return 0;
    gsize offset=(gsize)(g_utf8_offset_to_pointer(p->text,position)-p->text);
    memmove(p->text+offset+size,p->text+offset,p->bytes-offset+1);
    memcpy(p->text+offset,text,size); p->bytes+=size;
    gtk_entry_buffer_emit_inserted_text(buffer,position,text,accepted);
    return accepted;
}
static guint pin_delete(GtkEntryBuffer *buffer,guint position,guint chars) {
    PinBuffer *p=(PinBuffer *)buffer;
    gchar *start=g_utf8_offset_to_pointer(p->text,position);
    gchar *end=g_utf8_offset_to_pointer(start,chars);
    gsize size=(gsize)(end-start),tail=p->bytes-(gsize)(end-p->text);
    memmove(start,end,tail+1); p->bytes-=size;
    wipe(p->text+p->bytes+1,size);
    gtk_entry_buffer_emit_deleted_text(buffer,position,chars);
    return chars;
}
static void pin_finalize(GObject *object) {
    PinBuffer *p=(PinBuffer *)object; wipe(p->text,sizeof(p->text)); p->bytes=0;
    G_OBJECT_CLASS(pin_buffer_parent_class)->finalize(object);
}
static void pin_buffer_class_init(PinBufferClass *klass) {
    GtkEntryBufferClass *base=GTK_ENTRY_BUFFER_CLASS(klass);
    base->get_text=pin_text; base->get_length=pin_length;
    base->insert_text=pin_insert; base->delete_text=pin_delete;
    G_OBJECT_CLASS(klass)->finalize=pin_finalize;
}
static void pin_buffer_init(PinBuffer *p) { p->bytes=0; memset(p->text,0,sizeof(p->text)); }
GtkEntryBuffer *secure_pin_buffer_new(void) { return g_object_new(pin_buffer_get_type(),NULL); }

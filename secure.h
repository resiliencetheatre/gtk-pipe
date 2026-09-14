#ifndef GTK_PIPE_SECURE_H
#define GTK_PIPE_SECURE_H
#include <gtk/gtk.h>
/* HSPUI1 local protocol, deliberately independent of hsmproxy headers. */
enum { SEC_WAIT_CARD, SEC_PIN_REQUIRED, SEC_AUTHENTICATING, SEC_WAIT_PEER,
       SEC_CONNECTING, SEC_ESTABLISHED, SEC_ERROR, SEC_STOPPED };
typedef struct {
    guint state, reason, ports[3], mtu;
    guint64 generation, tx[3], rx[3], rejected, oversize, mtu_drops;
} SecureStatus;
gboolean secure_status_parse(const char *, gsize, SecureStatus *);
GtkEntryBuffer *secure_pin_buffer_new(void);
typedef struct Secure Secure;
typedef void (*SecureChanged)(gpointer, const SecureStatus *);
Secure *secure_new(GtkWindow *, const char *, const char *, SecureChanged, gpointer);
GtkWidget *secure_widget(Secure *);
GtkWidget *secure_status_widget(Secure *);
void secure_start(Secure *);
void secure_free(Secure *);
#endif

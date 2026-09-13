#include "secure.h"
#include <errno.h>
#include <string.h>

gboolean secure_status_parse(const char *data, gsize size, SecureStatus *out)
{
    if (!size || size > 511 || memchr(data, 0, size)) return FALSE;
    gchar *copy = g_strndup(data, size);
    gchar **parts = g_strsplit(copy, " ", -1);
    guint64 v[16];
    gboolean ok = g_strv_length(parts) == 17 && !strcmp(parts[0], "HSPUI1");
    for (guint i = 0; ok && i < 16; i++) {
        const char *p = parts[i + 1];
        if (!*p) { ok = FALSE; break; }
        for (const char *c = p; *c; c++) if (!g_ascii_isdigit(*c)) ok = FALSE;
        char *end; errno = 0;
        v[i] = g_ascii_strtoull(p, &end, 10);
        if (errno || *end) ok = FALSE;
    }
    if (ok) {
        ok = v[0] <= SEC_STOPPED && v[1] <= 12 && v[5] >= 64 && v[5] <= 1400;
        for (guint i = 2; i < 5; i++) if (!v[i] || v[i] > 65535) ok = FALSE;
        if (v[2] == v[3] || v[2] == v[4] || v[3] == v[4]) ok = FALSE;
        if (v[0] == SEC_ESTABLISHED && v[1] != 0) ok = FALSE;
    }
    if (ok) {
        *out = (SecureStatus){ .state=v[0], .reason=v[1],
            .ports={v[2],v[3],v[4]}, .mtu=v[5], .generation=v[6],
            .tx={v[7],v[8],v[9]}, .rx={v[10],v[11],v[12]},
            .rejected=v[13], .oversize=v[14], .mtu_drops=v[15] };
    }
    g_strfreev(parts); g_free(copy);
    return ok;
}

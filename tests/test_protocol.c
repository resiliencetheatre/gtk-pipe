#include "secure.h"
#include <string.h>
int main(void) {
    SecureStatus s;
    const char *valid="HSPUI1 5 0 5000 5002 5004 1100 7 1 2 3 4 5 6 0 8 9";
    g_assert_true(secure_status_parse(valid,strlen(valid),&s));
    g_assert_cmpuint(s.state,==,SEC_ESTABLISHED);
    g_assert_cmpuint(s.mtu,==,1100); g_assert_cmpuint(s.rx[2],==,6);
    const char *bad[]={
        "HSPUI2 5 0 5000 5002 5004 1100 7 1 2 3 4 5 6 0 8 9",
        "HSPUI1 5 0 5000 5000 5004 1100 7 1 2 3 4 5 6 0 8 9",
        "HSPUI1 5 1 5000 5002 5004 1100 7 1 2 3 4 5 6 0 8 9",
        "HSPUI1 5 0 5000 5002 5004 65536 7 1 2 3 4 5 6 0 8 9",
        "HSPUI1 5 0 5000 5002 5004 1100 -1 1 2 3 4 5 6 0 8 9",
        "HSPUI1 5 0 5000 5002 5004 1100 18446744073709551616 1 2 3 4 5 6 0 8 9",
        "HSPUI1 5 0 5000 5002 5004 1100 7 1 2 3 4 5 6 0 8 9 trailing",
        "HSPUI1 5 0 5000 5002 5004 1100 7 1 2 3 4 5 6 0 8",
        "HSPUI1 5 0 5000 5002 5004 1100 7 1 2 3 4 5 6 0 8 9\n"};
    for(guint i=0;i<G_N_ELEMENTS(bad);i++) g_assert_false(secure_status_parse(bad[i],strlen(bad[i]),&s));
    g_assert_false(secure_status_parse(valid,strlen(valid)+1,&s));
    char long_message[1024]; memset(long_message,'1',sizeof(long_message));
    g_assert_false(secure_status_parse(long_message,sizeof(long_message),&s));
    g_print("secure protocol: valid snapshot, malformed/version/overflow/truncation rejection passed\n");
}

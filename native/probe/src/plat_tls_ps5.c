// plat_tls.h on the PS5: none yet. The game's own HTTPS transfers that nnssl.c serves (ghost data) need a TLS client,
// which is OpenSSL loaded at run time on Linux and Schannel on Windows; the title can load neither. Every connection
// fails to set up and says why, so nnssl.c answers the game as it does on a Linux without libssl. The console's own
// TLS (libSceSsl) is the way to add it.
#include "plat_tls.h"

#include <stdio.h>

PlatTls *plat_tls_new(PlatSock s, const char *host, const unsigned char *ca, size_t ca_len, const unsigned char *alpn,
                      size_t alpn_len, char *error, size_t error_cap) {
    (void)s, (void)host, (void)ca, (void)ca_len, (void)alpn, (void)alpn_len;
    if (error && error_cap) snprintf(error, error_cap, "no TLS client on the PS5 build");
    return NULL;
}
int plat_tls_handshake(PlatTls *t) { (void)t; return PLAT_TLS_FAIL; }
long plat_tls_read(PlatTls *t, void *buf, size_t n, int peek) { (void)t, (void)buf, (void)n, (void)peek; return PLAT_TLS_FAIL; }
long plat_tls_write(PlatTls *t, const void *buf, size_t n) { (void)t, (void)buf, (void)n; return PLAT_TLS_FAIL; }
size_t plat_tls_pending(PlatTls *t) { (void)t; return 0; }
const char *plat_tls_alpn(PlatTls *t) { (void)t; return ""; }
const char *plat_tls_version(PlatTls *t) { (void)t; return ""; }
const char *plat_tls_error(PlatTls *t) { (void)t; return "no TLS client on the PS5 build"; }
void plat_tls_free(PlatTls *t) { (void)t; }

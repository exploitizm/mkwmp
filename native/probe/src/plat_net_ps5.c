// plat_net.h's resolver on the PS5: the console's own (libSceNet's sceNetResolver*). The title's C library has a
// getaddrinfo but no resolver configuration to use (no /etc/resolv.conf), so names go to the system's resolver, which
// follows the console's network settings. The rest of plat_net.h is plat_net_posix.c's, as on Linux.
//
// Declarations: libSceNet exports these (the payload SDK's libSceNet.so stub lists them) but its headers declare none.
// The signatures are the public PS4 SDK's, which the console keeps; SceNetInAddr is a 32-bit address in network order,
// the layout of struct in_addr. A timeout and retry count of 0 take the system's defaults.
#include "plat_net.h"

#include <pthread.h>
#include <stdio.h>

int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char *host, struct in_addr *out, int timeout, int retry, int flags);
int sceNetResolverDestroy(int resolver);

enum { POOL_BYTES = 16 * 1024 };   // a lookup's working memory: one name, one IPv4 answer

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_pool = -1;

static void pool_init(void) {
    sceNetInit();   // the title may have done it already; a second call changes nothing
    g_pool = sceNetPoolCreate("mkwmp-resolver", POOL_BYTES, 0);
}

int plat_resolve_ipv4(const char *host, struct in_addr *out) {
    if (!host || !*host) return 0;
    if (inet_pton(AF_INET, host, out) == 1) return 1;
    pthread_once(&g_once, pool_init);
    if (g_pool < 0) return 0;
    int r = sceNetResolverCreate("mkwmp", g_pool, 0);   // one per lookup: lookups on several threads never share one
    if (r < 0) return 0;
    struct in_addr a = {0};
    int ok = sceNetResolverStartNtoa(r, host, &a, 0, 0, 0) >= 0 && a.s_addr != 0;
    sceNetResolverDestroy(r);
    if (ok) *out = a;
    return ok;
}

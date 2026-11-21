// Sockets for the platform-neutral core (plat.h's sibling; include this one instead of plat.h where sockets are used,
// since Winsock must come before windows.h).
//
// The BSD calls themselves (socket, connect, bind, send, recv, sendto, recvfrom, getsockname, setsockopt, getsockopt,
// getaddrinfo, inet_addr, inet_ntoa, htons...) exist on both systems and are used directly. What differs is here:
// startup, close, non-blocking mode, timeouts, poll, scatter/gather I/O, shutdown and the error codes.
#ifndef MKWMP_PLAT_NET_H
#define MKWMP_PLAT_NET_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET PlatSock;
#define PLAT_NO_SOCK INVALID_SOCKET
typedef int plat_socklen_t;
typedef WSAPOLLFD PlatPollFd;
#define PLAT_POLLIN POLLRDNORM
#define PLAT_POLLOUT POLLWRNORM
#define PLAT_DONTWAIT 0   // ioctlsocket(FIONBIO) holds on Winsock
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
typedef int PlatSock;
#define PLAT_NO_SOCK (-1)
typedef socklen_t plat_socklen_t;
typedef struct pollfd PlatPollFd;
#define PLAT_POLLIN POLLIN
#define PLAT_POLLOUT POLLOUT
// Every read and send of a socket meant not to wait passes this: the PS5 ignores O_NONBLOCK on its sockets (a recv
// with nothing to read blocked; mkw-recomp's runtime/net/net_udp.c found the same), so plat_sock_nonblock alone is
// not enough there. On Linux the socket is non-blocking already and the flag changes nothing.
#define PLAT_DONTWAIT MSG_DONTWAIT
#endif

#include "plat.h"

#include <stddef.h>

void plat_net_init(void);                              // once per process (WSAStartup); safe to call again
void plat_sock_close(PlatSock s);
int plat_sock_nonblock(PlatSock s);                    // 0 on success
void plat_sock_timeouts(PlatSock s, uint32_t ms);     // send and receive timeouts of a blocking socket
void plat_sock_shutdown(PlatSock s);                   // both directions
int plat_poll(PlatPollFd *fds, int n, int timeout_ms); // as poll / WSAPoll: ready count, 0 timeout, -1 error

// The last socket call's failure, by meaning (WSAGetLastError / errno)
enum {
    PLAT_NET_OTHER,
    PLAT_NET_WOULDBLOCK,   // WSAEWOULDBLOCK / EAGAIN / EWOULDBLOCK
    PLAT_NET_INPROGRESS,   // a non-blocking connect started
    PLAT_NET_ALREADY,      // ... and is still going
    PLAT_NET_ISCONN,       // ... and is done
    PLAT_NET_INTR,
    PLAT_NET_CONNRESET,    // UDP: an earlier datagram's ICMP port-unreachable (WSAECONNRESET / ECONNREFUSED)
};
int plat_net_error(void);

// A host's IPv4 address: a dotted quad as it is, else a name lookup (getaddrinfo; on the PS5 the console's own
// resolver, plat_net_ps5.c, since the title's C library has no resolver configuration). 1 found, 0 not.
int plat_resolve_ipv4(const char *host, struct in_addr *out);

// Scatter / gather on a connected socket (WSASend / WSARecv, sendmsg / recvmsg): bytes moved, or -1
typedef struct { void *base; size_t len; } PlatIov;
long plat_sock_sendv(PlatSock s, const PlatIov *v, int n);
long plat_sock_recvv(PlatSock s, PlatIov *v, int n);

#endif

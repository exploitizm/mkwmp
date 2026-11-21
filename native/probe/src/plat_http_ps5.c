// plat_http.h on the PS5: plain HTTP/1.1 over a socket. The title links no libcurl and loads no libraries, and the
// seat server's API is small (a few JSON requests, seatapi.c), so a request is written by hand:
//   one connection per request (Connection: close), one deadline for the whole of it (connect, send, receive);
//   the answer's headers are read up to 16 KiB, its body (Content-Length, chunked, or up to the close) cut at the
//   caller's buffer as libcurl's sink does, and any byte past what was asked for is never stored.
// https:// is refused (status 0, plat_http_error says why): the PS5 build reaches a seat server over http://, which
// is the server's default (docs/HOSTING.md). The console's own HTTPS (libSceHttp2 with libSceSsl) is the way to add it.
#include "plat_http.h"

#include "plat_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { HEAD_MAX = 16 * 1024, HOST_MAX = 255, PATH_MAX_ = 2048 };

static _Thread_local char g_error[160];

const char *plat_http_error(void) { return g_error; }

#define FAIL(...) (snprintf(g_error, sizeof g_error, __VA_ARGS__), 0)

typedef struct {
    char host[HOST_MAX + 1], path[PATH_MAX_ + 1];
    unsigned port;
} Url;

static int parse_url(const char *url, Url *u) {
    if (!strncasecmp(url, "https://", 8)) return FAIL("https is not available on the PS5 build: use an http:// server");
    if (strncasecmp(url, "http://", 7)) return FAIL("not an http:// address");
    const char *h = url + 7, *end = h + strcspn(h, ":/?#");
    size_t n = (size_t)(end - h);
    if (!n || n > HOST_MAX) return FAIL("bad host in the server address");
    memcpy(u->host, h, n), u->host[n] = 0;
    u->port = 80;
    if (*end == ':') {
        char *after;
        unsigned long p = strtoul(end + 1, &after, 10);
        if (after == end + 1 || !p || p > 65535 || (*after && *after != '/' && *after != '?')) return FAIL("bad port");
        u->port = (unsigned)p, end = after;
    }
    const char *path = *end ? end : "/";
    if (*path == '?') return FAIL("bad path");   // a query needs a path before it
    if (strlen(path) > PATH_MAX_ || strpbrk(path, " \r\n")) return FAIL("bad path in the server address");
    snprintf(u->path, sizeof u->path, "%s", path);
    return 1;
}

// "chunked" anywhere in a Transfer-Encoding value, any case (the title's C library has no strcasestr)
static int has_chunked(const char *v) {
    for (; *v; v++)
        if (!strncasecmp(v, "chunked", 7)) return 1;
    return 0;
}

static uint64_t left_ms(uint64_t deadline) {
    uint64_t now = plat_ms();
    return now >= deadline ? 0 : deadline - now;
}

// Waits for events on s until the deadline: 1 ready, 0 out of time or an error.
static int wait_for(PlatSock s, short events, uint64_t deadline) {
    for (;;) {
        uint64_t ms = left_ms(deadline);
        if (!ms) return 0;
        PlatPollFd p = {.fd = s, .events = events};
        int r = plat_poll(&p, 1, (int)(ms > 0x7fffffff ? 0x7fffffff : ms));
        if (r > 0) return 1;
        if (r == 0 || plat_net_error() != PLAT_NET_INTR) return 0;
    }
}

static PlatSock connect_to(const Url *u, uint64_t deadline) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)u->port);
    if (!plat_resolve_ipv4(u->host, &a.sin_addr)) return FAIL("cannot resolve %s", u->host), PLAT_NO_SOCK;
    PlatSock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == PLAT_NO_SOCK) return FAIL("socket: %d", errno), PLAT_NO_SOCK;
    plat_sock_nonblock(s);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) {
        int e = plat_net_error();
        int err = 0;
        plat_socklen_t len = sizeof err;
        if ((e != PLAT_NET_INPROGRESS && e != PLAT_NET_WOULDBLOCK) || !wait_for(s, PLAT_POLLOUT, deadline) ||
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) || err) {
            FAIL("cannot connect to %s:%u (%d)", u->host, u->port, err ? err : errno);
            plat_sock_close(s);
            return PLAT_NO_SOCK;
        }
    }
    return s;
}

static int send_all(PlatSock s, const char *p, size_t n, uint64_t deadline) {
    while (n) {
        if (!wait_for(s, PLAT_POLLOUT, deadline)) return FAIL("timed out sending");
        long w = send(s, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            int e = plat_net_error();
            if (e == PLAT_NET_WOULDBLOCK || e == PLAT_NET_INTR) continue;
            return FAIL("send: %d", errno);
        }
        p += w, n -= (size_t)w;
    }
    return 1;
}

// The connection's bytes, read as they come: a small buffer and the deadline.
typedef struct {
    PlatSock s;
    uint64_t deadline;
    char buf[4096];
    size_t at, len;
    int closed;
} Reader;

static int fill(Reader *r) {
    if (r->at < r->len) return 1;
    if (r->closed) return 0;
    for (;;) {
        if (!wait_for(r->s, PLAT_POLLIN, r->deadline)) return FAIL("timed out waiting for the answer");
        long n = recv(r->s, r->buf, sizeof r->buf, 0);
        if (n > 0) { r->at = 0, r->len = (size_t)n; return 1; }
        if (n == 0) { r->closed = 1; return 0; }
        int e = plat_net_error();
        if (e != PLAT_NET_WOULDBLOCK && e != PLAT_NET_INTR) return FAIL("recv: %d", errno);
    }
}

static int get_byte(Reader *r) { return fill(r) ? (unsigned char)r->buf[r->at++] : -1; }

// One line of at most cap - 1 bytes, without its CRLF; 0 at the end of the stream or for a longer line.
static int read_line(Reader *r, char *line, size_t cap, size_t *total) {
    size_t n = 0;
    for (;;) {
        int c = get_byte(r);
        if (c < 0) return 0;
        if (++*total > HEAD_MAX) return FAIL("answer headers too long");
        if (c == '\n') break;
        if (n + 1 >= cap) return FAIL("answer header line too long");
        line[n++] = (char)c;
    }
    if (n && line[n - 1] == '\r') n--;
    line[n] = 0;
    return 1;
}

typedef struct { char *out; size_t cap, got; } Sink;

static void keep(Sink *k, const char *p, size_t n) {   // what does not fit is dropped, as libcurl's sink does
    size_t room = k->cap ? k->cap - 1 - k->got : 0;
    if (n > room) n = room;
    memcpy(k->out + k->got, p, n);
    k->got += n;
}

// n bytes of body (or, with until_close, all that comes); 1 when they all came.
static int body_bytes(Reader *r, Sink *k, unsigned long long n, int until_close) {
    while (until_close || n) {
        if (!fill(r)) return until_close && r->closed;
        size_t take = r->len - r->at;
        if (!until_close && take > n) take = (size_t)n;
        keep(k, r->buf + r->at, take);
        r->at += take;
        if (!until_close) n -= take;
    }
    return 1;
}

static int body_chunked(Reader *r, Sink *k) {
    char line[128];
    size_t total = 0;
    for (;;) {
        if (!read_line(r, line, sizeof line, &total)) return FAIL("bad chunk");
        total = 0;   // the limit is per line here: a body may hold many chunks
        char *end;
        unsigned long long size = strtoull(line, &end, 16);
        if (end == line || (*end && *end != ';')) return FAIL("bad chunk size");
        if (!size) return 1;   // trailers, if any, are not read: the connection closes
        if (!body_bytes(r, k, size, 0)) return FAIL("answer cut short");
        if (!read_line(r, line, sizeof line, &total) || line[0]) return FAIL("bad chunk end");
    }
}

PlatHttpResult plat_http(const char *method, const char *url, const char *bearer, const char *json, char *out,
                         size_t cap, int timeout_ms) {
    PlatHttpResult res = {0};
    if (cap) out[0] = 0;
    g_error[0] = 0;
    Url u;
    if (!method || !url || strpbrk(method, " \r\n") || (bearer && strpbrk(bearer, "\r\n")) || !parse_url(url, &u)) {
        if (!g_error[0]) FAIL("bad request");
        return res;
    }
    uint64_t deadline = plat_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 1);
    plat_net_init();
    PlatSock s = connect_to(&u, deadline);
    if (s == PLAT_NO_SOCK) return res;

    size_t body = json ? strlen(json) : 0;
    char head[PATH_MAX_ + HOST_MAX + 512];
    char host[HOST_MAX + 8];
    snprintf(host, sizeof host, u.port == 80 ? "%s" : "%s:%u", u.host, u.port);
    int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: mkwmp\r\nConnection: close\r\n"
                     "Accept-Encoding: identity\r\n", method, u.path, host);
    if (bearer && *bearer && n > 0 && (size_t)n < sizeof head)
        n += snprintf(head + n, sizeof head - (size_t)n, "Authorization: Bearer %s\r\n", bearer);
    if ((json || !strcmp(method, "POST")) && n > 0 && (size_t)n < sizeof head)   // a POST without a body: length 0
        n += snprintf(head + n, sizeof head - (size_t)n, "%sContent-Length: %zu\r\n",
                      json ? "Content-Type: application/json\r\n" : "", body);
    if (n > 0 && (size_t)n < sizeof head) n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    if (n <= 0 || (size_t)n >= sizeof head) {
        FAIL("request too long");
        plat_sock_close(s);
        return res;
    }
    if (!send_all(s, head, (size_t)n, deadline) || (body && !send_all(s, json, body, deadline))) {
        plat_sock_close(s);
        return res;
    }

    static _Thread_local Reader r;   // 4 KiB: kept off the stack of the caller's thread
    memset(&r, 0, sizeof r);
    r.s = s, r.deadline = deadline;
    char line[1024];
    size_t total = 0;
    int status = 0, chunked = 0, retry_after = 0, have_length = 0;
    unsigned long long length = 0;
    if (!read_line(&r, line, sizeof line, &total) || sscanf(line, "HTTP/1.%*1[01] %3d", &status) != 1 ||
        status < 100 || status > 999) {
        if (!g_error[0]) FAIL("not an HTTP answer");
        plat_sock_close(s);
        return res;
    }
    for (;;) {
        if (!read_line(&r, line, sizeof line, &total)) {
            if (!g_error[0]) FAIL("answer headers cut short");
            plat_sock_close(s);
            return res;
        }
        if (!line[0]) break;
        if (!strncasecmp(line, "Retry-After:", 12)) retry_after = atoi(line + 12);
        else if (!strncasecmp(line, "Content-Length:", 15)) {
            char *end;
            length = strtoull(line + 15, &end, 10), have_length = 1;
        } else if (!strncasecmp(line, "Transfer-Encoding:", 18) && has_chunked(line + 18)) chunked = 1;
    }
    Sink k = {out, cap, 0};
    int whole = !strcmp(method, "HEAD") || status == 204 || status == 304 ? 1
              : chunked ? body_chunked(&r, &k)
              : have_length ? body_bytes(&r, &k, length, 0)
              : body_bytes(&r, &k, 0, 1);
    plat_sock_close(s);
    if (cap) out[k.got] = 0;
    if (!whole) {   // as libcurl: a body cut short is a failed request
        if (!g_error[0]) FAIL("answer cut short");
        if (cap) out[0] = 0;
        return res;
    }
    res.status = status;
    res.retry_after = retry_after;
    res.length = k.got;
    return res;
}

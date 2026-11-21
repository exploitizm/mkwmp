// plat_http_ps5.c, the PS5's hand-written HTTP client, on Linux against tests/http_cases.py: what libcurl gives the
// seat client (status, body cut at the buffer, Retry-After) for every kind of answer, and status 0 with a reason for
// every failure (https, a bad address, a refused connection, a timeout, headers too long, a body cut short).
//
//   cc -O1 -Wall -Wextra -Werror -o test_http_ps5 tests/test_http_ps5.c src/plat_http_ps5.c src/plat_net_posix.c
//   python -I tests/http_cases.py PORTFILE &   then   ./test_http_ps5 http://127.0.0.1:<port>
// (build_plugin.py --test does both.)
#include "../src/plat_http.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf(" (error: %s)\n", plat_http_error());                                                               \
        }                                                                                                              \
    } while (0)

static char base[128];

static PlatHttpResult get(const char *path, char *out, size_t cap, int timeout_ms) {
    char url[256];
    snprintf(url, sizeof url, "%s%s", base, path);
    return plat_http("GET", url, NULL, NULL, out, cap, timeout_ms);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s http://127.0.0.1:PORT\n", argv[0]);
        return 2;
    }
    snprintf(base, sizeof base, "%s", argv[1]);
    char out[64];
    PlatHttpResult r;

    r = get("/len", out, sizeof out, 2000);
    CHECK(r.status == 200 && r.length == 5 && !strcmp(out, "hello") && r.retry_after == 7, "/len: %d %zu '%s' %d",
          r.status, r.length, out, r.retry_after);

    r = get("/chunked", out, sizeof out, 2000);
    CHECK(r.status == 201 && !strcmp(out, "hello world"), "/chunked: %d '%s'", r.status, out);

    r = get("/close", out, sizeof out, 2000);
    CHECK(r.status == 200 && !strcmp(out, "until the end"), "/close: %d '%s'", r.status, out);

    r = get("/big", out, sizeof out, 5000);   // the body cut at the buffer, the answer still whole
    CHECK(r.status == 200 && r.length == sizeof out - 1 && strlen(out) == sizeof out - 1, "/big: %d %zu", r.status,
          r.length);

    r = get("/missing", out, sizeof out, 2000);
    CHECK(r.status == 404 && r.length == 0 && !out[0], "/missing: %d", r.status);

    r = get("/longhead", out, sizeof out, 2000);
    CHECK(r.status == 0 && strstr(plat_http_error(), "too long"), "/longhead: %d", r.status);

    r = get("/short", out, sizeof out, 2000);
    CHECK(r.status == 0 && !out[0], "/short: %d '%s'", r.status, out);

    r = get("/slow", out, sizeof out, 300);
    CHECK(r.status == 0 && strstr(plat_http_error(), "timed out"), "/slow: %d", r.status);

    // the request as sent: method, path, Host, bearer, JSON body and its length
    char echo[1024];
    char url[256];
    snprintf(url, sizeof url, "%s/echo", base);
    r = plat_http("POST", url, "tok123", "{\"a\":1}", echo, sizeof echo, 2000);
    CHECK(r.status == 409, "/echo status %d", r.status);
    CHECK(strstr(echo, "POST /echo HTTP/1.1\r\n") && strstr(echo, "Host: 127.0.0.1:") &&
              strstr(echo, "Authorization: Bearer tok123\r\n") && strstr(echo, "Content-Type: application/json\r\n") &&
              strstr(echo, "Content-Length: 7\r\n") && strstr(echo, "\r\n\r\n{\"a\":1}"),
          "/echo request:\n%s", echo);
    r = plat_http("POST", url, NULL, NULL, echo, sizeof echo, 2000);   // a POST without a body says length 0
    CHECK(r.status == 409 && strstr(echo, "Content-Length: 0\r\n") && !strstr(echo, "Authorization"), "empty POST");

    // failures before any byte is sent
    r = plat_http("GET", "https://127.0.0.1/", NULL, NULL, out, sizeof out, 500);
    CHECK(r.status == 0 && strstr(plat_http_error(), "https"), "https");
    r = plat_http("GET", "http://127.0.0.1:99999/", NULL, NULL, out, sizeof out, 500);
    CHECK(r.status == 0 && strstr(plat_http_error(), "port"), "bad port");
    r = plat_http("GET", "http:///", NULL, NULL, out, sizeof out, 500);
    CHECK(r.status == 0 && strstr(plat_http_error(), "host"), "no host");
    r = plat_http("GET", "http://127.0.0.1:1/", NULL, NULL, out, sizeof out, 500);   // nothing listens on port 1
    CHECK(r.status == 0 && strstr(plat_http_error(), "connect"), "refused");
    r = plat_http("GET", "http://127.0.0.1/a b", NULL, NULL, out, sizeof out, 500);
    CHECK(r.status == 0 && strstr(plat_http_error(), "path"), "space in the path");
    r = plat_http("GET", base, "bad\r\nX-Injected: 1", NULL, out, sizeof out, 500);
    CHECK(r.status == 0, "a bearer with a line break is refused");

    printf("test_http_ps5: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}

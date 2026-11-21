"""A raw HTTP server of awkward answers for plat_http_ps5.c's test (tests/test_http_ps5.c): each path is one case the
PS5's hand-written client must handle as libcurl would. Plain sockets, not http.server, so the answers can be wrong on
purpose (cut short, headers too long, never sent).

    python -I http_cases.py PORTFILE      writes the port it listens on into PORTFILE, serves until killed

Paths: /len (Content-Length and Retry-After), /chunked, /close (body up to the close), /big (100 000 bytes),
/longhead (a 20 KiB header line), /short (fewer bytes than its length), /slow (never answers), /echo (the request
back as the body).
"""
import socket
import sys
import threading
import time


def read_request(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        got = conn.recv(4096)
        if not got:
            return None, b""
        data += got
    head, rest = data.split(b"\r\n\r\n", 1)
    length = 0
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1])
    while len(rest) < length:
        got = conn.recv(4096)
        if not got:
            break
        rest += got
    return head, rest


def answer(conn, head, body):
    path = head.split(b" ")[1]
    if path == b"/len":
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nRetry-After: 7\r\n\r\nhello")
    elif path == b"/chunked":
        conn.sendall(b"HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nhel\r\n")
        time.sleep(0.05)   # the second chunk in another segment
        conn.sendall(b"8;ext=1\r\nlo world\r\n0\r\n\r\n")
    elif path == b"/close":
        conn.sendall(b"HTTP/1.0 200 OK\r\n\r\nuntil the end")
    elif path == b"/big":
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n" + b"x" * 100000)
    elif path == b"/longhead":
        conn.sendall(b"HTTP/1.1 200 OK\r\nX-Long: " + b"a" * 20000 + b"\r\n\r\nbody")
    elif path == b"/short":
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc")
    elif path == b"/slow":
        time.sleep(3)
    elif path == b"/echo":
        echo = head + b"\r\n\r\n" + body
        conn.sendall(b"HTTP/1.1 409 Conflict\r\nContent-Length: %d\r\n\r\n" % len(echo) + echo)
    else:
        conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")


def serve(conn):
    with conn:
        try:
            head, body = read_request(conn)
            if head:
                answer(conn, head, body)
        except OSError:
            pass


def main():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(16)
    with open(sys.argv[1], "w") as f:
        f.write(str(srv.getsockname()[1]))
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=serve, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()

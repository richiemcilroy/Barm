#!/usr/bin/env python3
"""HTTPS servers for tests/fetch (native_tls.tov): three listeners on 127.0.0.1, using a
certificate for localhost/127.0.0.1 signed by ca.pem (the test CA), an expired one, and a
self-signed one. Prints "PORTS <good> <expired> <self>", then serves until killed.
The certificates last until 2126; ca.pem's private key isn't kept."""
import http.server, os, ssl, sys, threading

HERE = os.path.dirname(os.path.abspath(__file__))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send(self, status, body, headers=()):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.served = getattr(self, "served", 0) + 1
        path, _, query = self.path.partition("?")
        if path == "/text":
            self.send(200, b"secure hello")
        elif path == "/conn":
            self.send(200, f"request {self.served} on this connection".encode())
        elif path == "/big":
            n = int(query.split("=")[1]) if "=" in query else 1000
            self.send(200, bytes(97 + i % 26 for i in range(n)))
        elif path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"tls ", b"chunked ", b"body"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        elif path == "/redirect":
            self.send(302, b"", [("Location", query.split("=", 1)[1])])
        else:
            self.send(404, b"no such route")

    def do_POST(self):
        self.served = getattr(self, "served", 0) + 1
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.send(200, b"got " + body)


class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 128

    def handle_error(self, request, client_address):
        pass  # rejected handshakes are what some tests are about


def listen(cert, key):
    srv = Server(("127.0.0.1", 0), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(HERE, cert), os.path.join(HERE, key))
    # handshakes happen in the connection's thread, not the accepting one
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True, do_handshake_on_connect=False)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1]


ports = [listen("server.pem", "server.key"), listen("expired.pem", "server.key"), listen("self.pem", "self.key")]
print("PORTS", *ports, flush=True)
threading.Event().wait()

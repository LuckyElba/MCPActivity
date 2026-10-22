#!/usr/bin/env python3
"""Opt-in localhost Streamable HTTP MCP metadata proxy (stdlib only)."""
import argparse
import datetime
import hashlib
import http.client
import http.server
import json
import threading
import time
from urllib.parse import urlsplit

HOP = {"connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
       "te", "trailer", "transfer-encoding", "upgrade", "host", "content-length"}

def serve(listen, upstream, logfile_path):
    target = urlsplit(upstream)
    if target.scheme not in ("http", "https") or not target.hostname or target.username or target.password:
        raise ValueError("Upstream must be an http(s) URL without embedded credentials")
    lock = threading.Lock()

    class Proxy(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            pass

        def relay(self):
            started = time.monotonic()
            body_length = int(self.headers.get("Content-Length", "0"))
            if body_length < 0 or body_length > 8 * 1024 * 1024:
                self.send_error(413)
                return
            body = self.rfile.read(body_length) if body_length else b""
            method_name = None
            request_id = None
            if body and self.headers.get("Content-Type", "").split(";", 1)[0].strip() == "application/json":
                try:
                    obj = json.loads(body)
                    if isinstance(obj, dict):
                        method_name = obj.get("method") if isinstance(obj.get("method"), str) else None
                        if "id" in obj:
                            request_id = hashlib.sha256(json.dumps(obj["id"], sort_keys=True).encode()).hexdigest()[:16]
                except (ValueError, TypeError):
                    pass
            conn_type = http.client.HTTPSConnection if target.scheme == "https" else http.client.HTTPConnection
            conn = conn_type(target.hostname, target.port, timeout=60)
            sent = 0
            status = 502
            try:
                path = target.path or "/"
                if target.query:
                    path += "?" + target.query
                headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP}
                headers["Host"] = target.netloc
                conn.request(self.command, path, body=body if body else None, headers=headers)
                response = conn.getresponse()
                status = response.status
                self.send_response_only(status, response.reason)
                for name, value in response.getheaders():
                    if name.lower() not in HOP:
                        self.send_header(name, value)
                self.send_header("Connection", "close")
                self.end_headers()
                while True:
                    chunk = response.read1(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    sent += len(chunk)
            except (OSError, http.client.HTTPException) as exc:
                if sent == 0:
                    try:
                        self.send_error(502, "Upstream unavailable")
                    except OSError:
                        pass
            finally:
                conn.close()
                self.close_connection = True
                event = {"timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                         "kind": "http_exchange", "direction": "client_to_server",
                         "http_method": self.command, "method": method_name, "id_hash": request_id,
                         "status": status, "bytes": body_length, "response_bytes": sent,
                         "duration_ms": round((time.monotonic() - started) * 1000, 2),
                         "error": status >= 400}
                with lock:
                    logfile.write(json.dumps(event, separators=(",", ":")) + "\n")
                    logfile.flush()

        do_POST = relay
        do_GET = relay
        do_DELETE = relay

    with open(logfile_path, "a", encoding="utf-8", buffering=1) as logfile:
        server = http.server.ThreadingHTTPServer(listen, Proxy)
        print(f"Listening on http://{listen[0]}:{server.server_address[1]}; forwarding to {upstream}", flush=True)
        try:
            server.serve_forever()
        finally:
            server.server_close()

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--listen-port", type=int, default=8766)
    p.add_argument("--upstream", required=True)
    p.add_argument("--log", required=True)
    options = p.parse_args()
    serve(("127.0.0.1", options.listen_port), options.upstream, options.log)

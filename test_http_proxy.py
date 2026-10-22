"""Smoke-test the optional localhost Streamable HTTP metadata proxy."""
import http.server
import json
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

class Upstream(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *_args): pass
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(b"event: message\\ndata: ping\\n\\n")
        self.wfile.flush()
        self.close_connection = True

    def do_POST(self):
        payload = self.rfile.read(int(self.headers["Content-Length"]))
        request = json.loads(payload)
        assert self.headers.get("Authorization") == "Bearer test"
        response = json.dumps({"jsonrpc":"2.0","id":request["id"],"result":{"ok":True}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.send_header("Mcp-Session-Id", "test-session")
        self.end_headers()
        self.wfile.write(response)

upstream = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
threading.Thread(target=upstream.serve_forever, daemon=True).start()
from socket import socket
with socket() as sock:
    sock.bind(("127.0.0.1", 0))
    proxy_port = sock.getsockname()[1]
with tempfile.TemporaryDirectory() as directory:
    log = Path(directory) / "events.jsonl"
    proxy = subprocess.Popen([sys.executable, str(Path(__file__).with_name("mcp_http_proxy.py")),
                              "--listen-port", str(proxy_port), "--upstream",
                              "http://127.0.0.1:%d/mcp" % upstream.server_port,
                              "--log", str(log)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        for attempt in range(30):
            try:
                body = b'{"jsonrpc":"2.0","id":5,"method":"tools/list"}'
                req = urllib.request.Request("http://127.0.0.1:%d/mcp" % proxy_port, data=body,
                            headers={"Content-Type":"application/json","Authorization":"Bearer test"},
                            method="POST")
                with urllib.request.urlopen(req, timeout=2) as response:
                    assert response.headers["Mcp-Session-Id"] == "test-session"
                    assert json.loads(response.read())["result"]["ok"]
                break
            except OSError:
                if attempt == 29: raise
                time.sleep(.1)
        with urllib.request.urlopen("http://127.0.0.1:%d/mcp" % proxy_port, timeout=3) as stream:
            assert stream.headers["Content-Type"] == "text/event-stream"
            assert b"data: ping" in stream.read()
        event = next(json.loads(line) for line in log.read_text().splitlines() if '"http_method":"POST"' in line)
        assert event["method"] == "tools/list" and event["status"] == 200
        assert "Authorization" not in str(event) and "result" not in str(event)
        assert event["response_bytes"] > 0
        print("HTTP proxy smoke test passed")
    finally:
        proxy.terminate()
        proxy.wait(timeout=4)
        upstream.shutdown()

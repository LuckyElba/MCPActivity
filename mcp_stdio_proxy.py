#!/usr/bin/env python3
"""Opt-in stdio MCP metadata proxy. Never writes diagnostics to stdout."""
import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()

def token(value):
    encoded = json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()[:16]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path, help="Local JSONL metadata destination")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- followed by server executable and args")
    args = parser.parse_args()
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    if not command:
        parser.error("Provide a server command after --")
    if args.log.exists() and args.log.is_dir():
        parser.error("Log destination must be a file")
    args.log.parent.mkdir(parents=True, exist_ok=True)
    log_lock = threading.Lock()
    pending = {}
    pending_lock = threading.Lock()
    with args.log.open("a", encoding="utf-8", buffering=1) as logfile:
        def record(event):
            event["timestamp"] = stamp()
            with log_lock:
                logfile.write(json.dumps(event, ensure_ascii=True, separators=(",", ":")) + "\n")

        def observe(line, direction):
            event = {"direction": direction, "bytes": len(line)}
            try:
                message = json.loads(line)
                if not isinstance(message, dict):
                    event["kind"] = "non_object"
                else:
                    event["kind"] = "notification" if "method" in message and "id" not in message else "message"
                    if "method" in message and isinstance(message["method"], str):
                        event["method"] = message["method"]
                    if "id" in message:
                        ref = token(message["id"])
                        event["id_hash"] = ref
                        if "method" in message:
                            with pending_lock:
                                pending[(direction, ref)] = time.monotonic()
                            event["kind"] = "request"
                        else:
                            event["kind"] = "response"
                            other = "server_to_client" if direction == "client_to_server" else "client_to_server"
                            with pending_lock:
                                start = pending.pop((other, ref), None)
                            if start is not None:
                                event["duration_ms"] = round((time.monotonic() - start) * 1000, 2)
                    if "error" in message:
                        event["error"] = True
            except (ValueError, UnicodeError):
                event["kind"] = "unparsed"
            record(event)

        child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None, bufsize=0)
        record({"kind": "server_started", "pid": child.pid})

        def forward(source, destination, direction):
            try:
                while True:
                    line = source.readline()
                    if not line:
                        break
                    destination.write(line)
                    destination.flush()
                    observe(line, direction)
            except (BrokenPipeError, OSError, ValueError) as exc:
                record({"kind": "stream_closed", "direction": direction, "error_type": type(exc).__name__})
            finally:
                try:
                    destination.close()
                except OSError:
                    pass

        to_server = threading.Thread(target=forward, args=(sys.stdin.buffer, child.stdin, "client_to_server"), daemon=True)
        to_client = threading.Thread(target=forward, args=(child.stdout, sys.stdout.buffer, "server_to_client"), daemon=True)
        to_server.start()
        to_client.start()
        to_client.join()
        if child.poll() is None:
            child.terminate()
        try:
            result = child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            result = child.wait()
        record({"kind": "server_stopped", "exit_code": result})
        return result

if __name__ == "__main__":
    sys.exit(main())

"""Dependency-free smoke test for the stdio metadata proxy."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as temporary:
    log = Path(temporary) / "activity.jsonl"
    server = Path(temporary) / "server.py"
    server.write_text(
        "import sys, json\n"
        "for line in sys.stdin.buffer:\n"
        "    item=json.loads(line)\n"
        "    sys.stdout.write(json.dumps({'jsonrpc':'2.0','id':item['id'],'result':{'ok':True}})+'\\n')\n"
        "    sys.stdout.flush()\n", encoding="utf-8"
    )
    payload = b'{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}\n'
    run = subprocess.run(
        [sys.executable, str(ROOT / "mcp_stdio_proxy.py"), "--log", str(log),
         "--", sys.executable, str(server)],
        input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15
    )
    assert run.returncode == 0, (run.returncode, run.stderr)
    response = json.loads(run.stdout)
    assert response["id"] == 1 and response["result"]["ok"]
    events = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
    request = next(item for item in events if item.get("kind") == "request")
    reply = next(item for item in events if item.get("kind") == "response")
    assert request["method"] == "tools/list"
    assert reply["id_hash"] == request["id_hash"]
    assert reply["duration_ms"] >= 0
    assert all("params" not in entry and "result" not in entry for entry in events)
    assert all("ok" not in entry for entry in events)
    print("Proxy smoke test passed:", len(events), "metadata events")

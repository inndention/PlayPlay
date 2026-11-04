"""Isolated release check: automation slots, slave mappings, recording and recomposition.
Uses a temporary station and a mock authenticated upstream; never touches live station data.
Run: python tests/integration_scene_automation.py
"""
import contextlib
import datetime as dt
import http.server
import json
import os
import pathlib
import socket
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.request
import urllib.error
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

def wait_for(fn, timeout=15):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = fn()
            if last:
                return last
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(.15)
    raise AssertionError(f"Timed out: {last}")

def main():
    with tempfile.TemporaryDirectory(prefix="playplay-release-") as folder:
        temp = pathlib.Path(folder)
        local_media = temp / "clip.mp4"
        local_media.write_bytes(b"media identity fixture")
        hour_start = int(time.time() // 3600 * 3600000)
        item = {"id": "remote-run", "itemId": "remote-item", "assetId": "remote-asset", "path": "Z:/remote/clip.mp4", "title": "Remote fixture", "duration": 3600, "inPoint": 0, "plannedStartMs": hour_start, "origin": "schedule"}
        upstream = {"nodeId": "release-upstream", "host": "RELEASE-UPSTREAM", "profile": {"id": "main", "timezone": "UTC", "extra": {}, "outputs": [{"type": "ndi", "name": "pgm", "sourceName": "Program"}]},
                    "state": {"state": "ON_AIR"}, "runlist": {"current": {"hourStartMs": hour_start, "items": [item]}, "next": {"hourStartMs": hour_start + 3600000, "items": []}},
                    "media": {item["path"]: {"hash": "fixture-hash", "hashKind": "quick", "size": local_media.stat().st_size}}}
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                assert self.headers.get("X-PP-Pass") == "test-password"
                data = json.dumps(upstream).encode()
                self.send_response(200); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
            def log_message(self, *_):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        port, ws_port = free_port(), free_port()
        ipc = "PlayPlayRelease-" + uuid.uuid4().hex[:12]
        with (temp / "supervisor.log").open("wb") as log:
            proc = subprocess.Popen([str(BUILD / "PlayPlaySupervisor.exe"), "--data-dir", str(temp / "station"), "--ipc-name", ipc, "--rest-port", str(port), "--ws-port", str(ws_port), "--health-port", "0", "--no-autostart", "--log-dir", "none"], cwd=BUILD, stdout=log, stderr=log)
            def api(path, body=None, method=None, expected=200):
                req = urllib.request.Request(f"http://127.0.0.1:{port}/api/v1{path}", data=json.dumps(body).encode() if body is not None else None, method=method, headers={"Content-Type": "application/json"})
                try:
                    with urllib.request.urlopen(req, timeout=4) as response:
                        assert response.status == expected
                        return json.load(response)
                except urllib.error.HTTPError as err:
                    assert err.code == expected, (err.code, err.read().decode())
                    return json.load(err)
            try:
                wait_for(lambda: api("/status"))
                # Show templates restore production setup and per-client layout without cutting program.
                original_map = {"slots": [{"title": "Camera 1", "path": "ndi://camera"}], "transition": {"type": "mix", "durationSec": 1}}
                api("/settings/ui.inputMap", {"value": original_map}, "PUT")
                template = api("/shows", {"name": "Evening News", "layout": {"web": [{"id": "rundown", "width": 4}]}}, "POST")
                assert template["inputMap"] == original_map
                updated = api("/shows", {"id": template["id"], "name": "Evening News", "layout": {"native": {"deck": 1}}}, "POST")
                assert "web" in updated["layout"] and "native" in updated["layout"]
                api("/settings/ui.inputMap", {"value": {"slots": []}}, "PUT")
                api("/shows/" + template["id"] + "/apply", {}, "POST")
                assert json.loads(api("/settings/ui.inputMap")["value"]) == original_map
                api("/shows/" + template["id"], None, "DELETE")
                api("/cg/html/main", {"output": 1, "template": "starnews_lower_third", "params": {"text1": "First"}}, "POST")
                api("/cg/html/main", {"output": 2, "template": "starnews_headline", "params": {"text1": "Second"}}, "POST")
                assert api("/cg/html/main?output=1")["template"] == "starnews_lower_third"
                assert api("/cg/html/main?output=2")["template"] == "starnews_headline"
                api("/cg/html/main/clear", {"output": 2}, "POST")
                assert api("/cg/html/main?output=2")["url"] == "" and api("/cg/html/main?output=1")["url"]
                api("/cg/html/main", {"output": 9, "template": "starnews_headline"}, "POST", 422)
                assert len(api("/automation/channels", {"count": 8}, "PUT")["channels"]) == 8
                api("/automation/channels", {"count": 9}, "PUT", 422)
                api("/automation/channels", {"count": 1.5}, "PUT", 422)
                cfg = api("/automation/channels", {"count": 2}, "PUT")
                assert sum(c["enabled"] for c in cfg["channels"]) == 2
                assert cfg["channels"][0]["id"] == "main"
                assert [c["profile"]["id"] for c in api("/status")["channels"]] == ["main", "auto2"]
                api("/media", {"id": "local-asset", "path": str(local_media), "title": "Local copy", "duration": 3600, "fileSizeBytes": local_media.stat().st_size, "hash": "fixture-hash", "hashKind": "quick"}, "POST")
                # MediaRecord preserves unknown fields as extra; explicitly provide the archive metadata too.
                api("/media/local-asset", {"fileSizeBytes": local_media.stat().st_size, "hash": "fixture-hash", "hashKind": "quick"}, "PUT")
                api("/remote/nodes", {"nodes": [{"id": "upstream", "name": "Upstream fixture", "url": f"http://127.0.0.1:{server.server_port}", "password": "test-password"}]}, "PUT")
                date = dt.datetime.fromtimestamp(hour_start / 1000, dt.timezone.utc)
                slot_id = f"{date.date()}-{date.hour}"
                original = {"channelId": "main", "date": str(date.date()), "hour": date.hour, "items": [{"id": "local-original", "title": "Original local hour", "path": str(local_media), "duration": 3600}]}
                api("/schedule/main/slot/" + slot_id, original, "PUT")
                api("/automation/channels/main", {"mode": "mirror", "nodeId": "missing", "channelId": "main"}, "PUT", 422)
                api("/automation/channels/main", {"mode": "mirror", "nodeId": "upstream", "channelId": "main"}, "PUT")
                def synced():
                    c = next(c for c in api("/automation/channels")["channels"] if c["id"] == "main")
                    return c if c.get("slaveStatus", {}).get("lastSyncMs") else None
                mapped = wait_for(synced)
                assert mapped["slave"]["mode"] == "mirror"
                mirrored = api("/schedule/main/slot/" + slot_id)
                assert mirrored["items"][0]["path"] == str(local_media), mirrored
                upstream["media"][item["path"]]["hash"] = "missing-media"
                wait_for(lambda: "Local media missing" in next(c for c in api("/automation/channels")["channels"] if c["id"] == "main").get("slaveStatus", {}).get("error", ""))
                assert api("/schedule/main/slot/" + slot_id)["items"] == mirrored["items"]
                upstream["media"][item["path"]]["hash"] = "fixture-hash"
                wait_for(synced)
                api("/automation/channels/main", {"mode": "local"}, "PUT")
                assert api("/schedule/main/slot/" + slot_id)["items"][0]["title"] == "Original local hour"
                api("/automation/channels/main", {"mode": "relay", "nodeId": "upstream", "channelId": "main"}, "PUT")
                wait_for(synced)
                relay = api("/channels/main")["profile"]["fixedSource"]
                assert relay["name"] == "RELEASE-UPSTREAM (Program)", relay
                api("/automation/channels/main", {"mode": "local"}, "PUT")
                api("/automation/channels", {"count": 1}, "PUT")
                api("/channels/main", {"width": 64, "height": 36, "outputs": [], "armed": False, "timezone": "UTC"}, "PUT")
                next_date = date + dt.timedelta(hours=1)
                next_id = f"{next_date.date()}-{next_date.hour}"
                next_slot = {"channelId": "main", "date": str(next_date.date()), "hour": next_date.hour, "items": [
                    {"id": "next-a", "title": "A", "path": str(local_media), "duration": 60},
                    {"id": "next-b", "title": "B", "path": str(local_media), "duration": 60}]}
                api("/schedule/main/slot/" + next_id, next_slot, "PUT")
                api("/channels/main/start", {}, "POST")
                wait_for(lambda: api("/channels/main")["process"]["connected"])
                api("/channels/main/arm", {}, "POST")
                api("/channels/main/load-hour", {}, "POST")
                try:
                    run = wait_for(lambda: (lambda r: r if len(r.get("next", {}).get("items", [])) >= 2 else None)(api("/channels/main/runlist")), timeout=5)
                except AssertionError:
                    raise AssertionError((api("/channels/main/runlist"), api("/channels/main"), api("/schedule/main/slot/" + next_id)))
                current_items = run["current"]["items"]
                if current_items:
                    current_anchor = current_items[0]
                    api("/channels/main/rundown/insert", {"anchorId": current_anchor["id"], "side": "before", "items": [{"title": "Forbidden", "path": str(local_media), "duration": 10}]}, "POST", 409)
                anchor = next(x for x in run["next"]["items"] if x["itemId"] == "next-b")
                drop = {"anchorId": anchor["id"], "side": "before", "items": [{"title": "Inserted", "path": str(local_media), "duration": 10}]}
                api("/channels/main/rundown/insert", drop, "POST")
                assert [x["title"] for x in api("/schedule/main/slot/" + next_id)["items"]] == ["A", "Inserted", "B"]
                api("/channels/main/rundown/insert", {**drop, "anchorId": "stale-id"}, "POST", 409)
                api("/channels/main/disarm", {}, "POST")
                session = str(temp / "capture")
                api("/channels/main/transport", {"action": "record", "params": {"op": "start", "directory": session}}, "POST")
                wait_for(lambda: api("/channels/main/state").get("recording", {}).get("frames", 0) >= 10)
                api("/channels/main/transport", {"action": "record", "params": {"op": "stop"}}, "POST")
                saved = wait_for(lambda: api("/channels/main/state").get("recording", {}).get("complete"))
                assert saved
                subprocess.run([str(BUILD / "PlayPlayRecompose.exe"), "--session", session, "--output", str(temp / "recomposed.mkv")], check=True, cwd=BUILD, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                meta = json.loads((temp / "capture/session.json").read_text())
                assert meta["complete"] and meta["frames"] >= 10
                job = api("/recordings/recompose", {"session": session, "output": str(temp / "job-export.mkv"), "edits": {"omitLayers": [30]}}, "POST")
                wait_for(lambda: api("/recordings/jobs/" + job["id"])["state"] == "complete")
                api("/recordings/recompose", {"session": session, "output": str(temp / "job-export.mkv")}, "POST", 409)
                # QML loads on all changed pages. The app's screenshot mode exits automatically.
                for page in ("remote", "inputs", "onair", "cg", "schedule"):
                    command = [str(BUILD / "PlayPlay.exe"), "--supervisor-port", str(port), "--supervisor-ipc", ipc, "--screenshot", str(temp / f"{page}.png"), "--page", page, "--delay", "3000", "--zoom", "1"]
                    if os.environ.get("PLAYPLAY_DEBUG_SCREENSHOT"):
                        command = [r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe", "-lines", "-c", "sxe av; g; .ecxr; kv; q"] + command
                    app = subprocess.run(command, cwd=BUILD, env=dict(os.environ, QT_FORCE_STDERR_LOGGING="1"), capture_output=True, timeout=40)
                    if os.environ.get("PLAYPLAY_DEBUG_SCREENSHOT"):
                        (ROOT / "output" / "debug-qml-shutdown.txt").write_bytes(app.stdout + app.stderr)
                        assert b"Access violation" not in app.stdout, (app.stdout + app.stderr).decode(errors="replace")[-8000:]
                    errors = app.stdout + app.stderr
                    assert app.returncode == 0 and (temp / f"{page}.png").exists(), f"Page {page}, exit {app.returncode}: " + errors.decode(errors="replace")[-5000:]
                    assert b"is not available" not in errors and b"Cannot assign to non-existent property" not in errors, errors.decode(errors="replace")[-3000:]
                    artifact_dir = BUILD / "artifacts/scene-automation"
                    artifact_dir.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(temp / f"{page}.png", artifact_dir / f"{page}.png")
                print("PASS: 1–8 slots, invalid counts, authenticated mirror, local-media mapping, schedule restoration, NDI relay, engine recording, MKV recomposition and native pages")
            finally:
                with contextlib.suppress(Exception):
                    api("/channels/main/stop", {}, "POST")
                proc.terminate()
                with contextlib.suppress(subprocess.TimeoutExpired):
                    proc.wait(timeout=5)
                if proc.poll() is None:
                    proc.kill(); proc.wait()
                server.shutdown()

if __name__ == "__main__":
    main()

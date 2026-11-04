#!/usr/bin/env python3
"""local_spot_jobs.py - create (or update) the local-spot render jobs on a running Supervisor.

    python scripts/local_spot_jobs.py                      # live Supervisor on :7700, channel ch2
    python scripts/local_spot_jobs.py --port 7750          # a test Supervisor
    python scripts/local_spot_jobs.py --run-now            # also render each job once right away
    python scripts/local_spot_jobs.py --dry-run            # print what would be sent

Jobs (see docs/spots.md, "Local spots"). Idempotent: a job is found by its label and updated, never duplicated.

  Now Playing        cinema        made at 00:05 04:05 ... 20:05, airs every 4 hours (hours 0 4 8 12 16 20) at :27
  Traffic Alerts     traffic       made every 60 min, airs every hour at :20
  Sports Scoreboard  sports_local  made at 02:05 06:05 ... 22:05, airs every 4 hours (hours 2 6 10 14 18 22) at :27
  Featured Home      realty        made every 60 min (a different home each time), airs every hour at :52
  Weather            weather_full  made every 60 min (animated 7-day cards, sunset, moon phase, burn safety, lake levels), airs every
                                   hour at :05 - this REPLACES the recipe of the existing "Weather" job (use --keep-weather to leave it)
  Market Close       markets_close made once after the US close (Mon-Fri 16:20), airs every 3 hours until the next
                                   open: 17 20 23 (Mon-Fri) and 02 05 08 (Tue-Sat) at :42

A job's GEN marker is only written into the weekly grid once the job has produced its first spot, so use --run-now (or wait for
the first scheduled run) and check GET /api/v1/spots/markers.
"""
import argparse
import json
import sys
import urllib.error
import urllib.request


def clock(hour0: int, minute: int, step: int = 4) -> list:
    return ["%02d:%02d" % (h, minute) for h in range(hour0, 24, step)]


def jobs_for(channel: str) -> list:
    def placement(hours, at_min):
        return [{"channel": channel, "hours": hours, "atMin": at_min, "dows": []}]
    return [
        # the hourly weather spot, upgraded: same placement (:05 every hour) as the original Weather job, found by its label and updated in place
        {"label": "Weather", "recipe": "weather_full", "channel": channel, "enabled": True,
         "every": {"kind": "interval", "minutes": 60}, "placements": placement("all", 5)},
        {"label": "Now Playing", "recipe": "cinema", "channel": channel, "enabled": True,
         "every": {"kind": "clock", "times": clock(0, 5)}, "placements": placement([0, 4, 8, 12, 16, 20], 27)},
        {"label": "Traffic Alerts", "recipe": "traffic", "channel": channel, "enabled": True,
         "every": {"kind": "interval", "minutes": 60}, "placements": placement("all", 20)},
        {"label": "Sports Scoreboard", "recipe": "sports_local", "channel": channel, "enabled": True,
         "every": {"kind": "clock", "times": clock(2, 5)}, "placements": placement([2, 6, 10, 14, 18, 22], 27)},
        # made at 16:20 on weekdays (the US close is 16:00); aired at 5, 8 and 11 PM that evening and 2, 5 and 8 AM the next morning, so a Friday
        # wrap airs through Saturday morning and nothing stale airs on Sunday or Monday morning. Holidays make no spot (the market did not trade).
        {"label": "Market Close", "recipe": "markets_close", "channel": channel, "enabled": True, "dows": [1, 2, 3, 4, 5],
         "every": {"kind": "clock", "times": ["16:20"]},
         "placements": [{"channel": channel, "hours": [17, 20, 23], "atMin": 42, "dows": [1, 2, 3, 4, 5]},
                        {"channel": channel, "hours": [2, 5, 8], "atMin": 42, "dows": [2, 3, 4, 5, 6]}]},
        {"label": "Featured Home", "recipe": "realty", "channel": channel, "enabled": True,
         "every": {"kind": "interval", "minutes": 60}, "placements": placement("all", 52)},
    ]


def call(base: str, method: str, path: str, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, method=method, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        raise SystemExit("%s %s -> HTTP %d: %s" % (method, path, e.code, e.read().decode("utf8", "replace")[:300]))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7700)
    ap.add_argument("--channel", default="ch2")
    ap.add_argument("--run-now", action="store_true", help="render each job once right away (async)")
    ap.add_argument("--disabled", action="store_true", help="create the jobs switched off")
    ap.add_argument("--only", nargs="+", metavar="LABEL", help="create / update only these jobs, and with --run-now render only these (labels as printed above)")
    ap.add_argument("--keep-weather", action="store_true", help="do not switch the existing Weather job to the animated forecast recipe")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    base = "http://%s:%d/api/v1" % (a.host, a.port)
    wanted = jobs_for(a.channel)
    if a.keep_weather:
        wanted = [j for j in wanted if j["label"] != "Weather"]
    if a.only:
        want = {x.lower() for x in a.only}
        unknown = want - {j["label"].lower() for j in wanted}
        if unknown:
            print("No such job label(s): %s" % ", ".join(sorted(unknown)), file=sys.stderr)
            return 2
        wanted = [j for j in wanted if j["label"].lower() in want]
    if a.disabled:
        for j in wanted:
            j["enabled"] = False
    if a.dry_run:
        print(json.dumps(wanted, indent=2))
        return 0
    recipes = {r["key"] for r in call(base, "GET", "/spots/recipes").get("recipes", [])}
    missing = [j["recipe"] for j in wanted if j["recipe"] not in recipes]
    if missing:
        print("This Supervisor does not know the recipe(s) %s: it is running a build older than the local spots." % ", ".join(missing), file=sys.stderr)
        return 2
    existing = {j.get("label"): j for j in call(base, "GET", "/spots/jobs").get("jobs", [])}
    for j in wanted:
        cur = existing.get(j["label"])
        saved = call(base, "PUT", "/spots/jobs/" + cur["id"], j) if cur else call(base, "POST", "/spots/jobs", j)
        print("%-18s %s  id %s  GEN %s  %s  every %s" % (j["label"], "updated" if cur else "created", saved.get("id"), saved.get("genNum"),
                                                          "on" if saved.get("enabled") else "OFF", "%d min" % saved.get("cadenceMin", 0)))
        if a.run_now and saved.get("enabled"):
            r = call(base, "POST", "/spots/jobs/%s/run" % saved["id"], {"wait": False})
            print("%-18s render queued (genId %s)" % ("", r.get("genId")))
    return 0


if __name__ == "__main__":
    sys.exit(main())

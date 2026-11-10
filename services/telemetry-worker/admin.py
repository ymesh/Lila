"""Credits admin for the telemetry Worker. Set LILA_CREDITS_ADMIN_TOKEN to the Worker's ADMIN_TOKEN secret.

  python admin.py state                                   everything: people, links, settings, reports, alerts
  python admin.py set contributors contributors.json      replace a hand-kept list (corporate, pastSponsors,
                                                          contributors, testers, hide, allowlist, fx, steamDlc,
                                                          founderOpen) with a JSON file
  python admin.py link person:github:eve https://github.com/eve [--force]
  python admin.py link contributor:"Sam Example" https://github.com/sam
  python admin.py pull person:github:eve [--ban] [--reason "redirected to a scam"]
  python admin.py person github:eve --name "Eve M" [--hidden 1] [--merge kofi:abc steam:123]
  python admin.py payment bank:2026-11-03 --donor manual:eve --cents 50000 [--name Eve --public]
  python admin.py reverse kofi:<transaction id>
  python admin.py publish | nightly
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

BASE = os.environ.get("LILA_CREDITS_WORKER", "https://lila-telemetry.lila3d.workers.dev")


def call(method, path, body=None):
    token = os.environ.get("LILA_CREDITS_ADMIN_TOKEN")
    if not token:
        sys.exit("set LILA_CREDITS_ADMIN_TOKEN")
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json",
                                          # Cloudflare rejects urllib's default User-Agent (error 1010).
                                          "User-Agent": "lila-credits-admin"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit(f"{e.code}: {e.read().decode()}")


def main():
    p = argparse.ArgumentParser(description="Lila credits admin")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("state")
    s = sub.add_parser("set")
    s.add_argument("key")
    s.add_argument("file")
    s = sub.add_parser("link")
    s.add_argument("owner")
    s.add_argument("url")
    s.add_argument("--force", action="store_true")
    s = sub.add_parser("pull")
    s.add_argument("owner")
    s.add_argument("--ban", action="store_true")
    s.add_argument("--reason", default="pulled by hand")
    s = sub.add_parser("person")
    s.add_argument("id")
    s.add_argument("--name")
    s.add_argument("--hidden", type=int)
    s.add_argument("--merge", nargs="*", default=[])
    s = sub.add_parser("payment")
    s.add_argument("id")
    s.add_argument("--donor", required=True)
    s.add_argument("--cents", type=int, required=True)
    s.add_argument("--at")
    s.add_argument("--name")
    s.add_argument("--public", action="store_true")
    s = sub.add_parser("reverse")
    s.add_argument("id")
    sub.add_parser("publish")
    sub.add_parser("nightly")
    a = p.parse_args()

    if a.cmd == "state":
        out = call("GET", "/admin/state")
    elif a.cmd == "set":
        with open(a.file, encoding="utf-8") as f:
            out = call("PUT", f"/admin/settings/{a.key}", {"value": json.load(f)})
    elif a.cmd == "link":
        out = call("POST", "/admin/link", {"owner": a.owner, "url": a.url, "force": a.force})
    elif a.cmd == "pull":
        out = call("POST", "/admin/pull", {"owner": a.owner, "ban": a.ban, "reason": a.reason})
    elif a.cmd == "person":
        body = {"id": a.id, "merge": a.merge}
        if a.name is not None:
            body["name"] = a.name
        if a.hidden is not None:
            body["hidden"] = bool(a.hidden)
        out = call("POST", "/admin/person", body)
    elif a.cmd == "payment":
        body = {"id": a.id, "donor_id": a.donor, "cents": a.cents, "public": a.public}
        if a.at:
            body["at"] = a.at
        if a.name:
            body["name"] = a.name
        out = call("POST", "/admin/payment", body)
    elif a.cmd == "reverse":
        out = call("POST", "/admin/payment", {"id": a.id, "reversed": True})
    else:
        out = call("POST", f"/admin/{a.cmd}", {})
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()

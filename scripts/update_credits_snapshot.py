"""Refreshes resources/credits/credits.json, the credits bundled with a release, from the live file. Links are stripped:
they only ever come from the live file, so a pulled link can't survive in an old release."""

import json
import pathlib
import sys
import urllib.request

URL = "https://lila-telemetry.lila3d.workers.dev/credits.json"
OUT = pathlib.Path(__file__).resolve().parent.parent / "resources" / "credits" / "credits.json"


def strip_links(value):
    if isinstance(value, dict):
        return {k: strip_links(v) for k, v in value.items() if k != "url"}
    if isinstance(value, list):
        return [strip_links(v) for v in value]
    return value


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else URL
    # Cloudflare refuses Python's default User-Agent.
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "lila-release"}), timeout=30) as r:
        credits = json.load(r)
    if not isinstance(credits, dict) or credits.get("version") != 1:
        sys.exit(f"unexpected credits file from {url}")
    credits.pop("updated", None)
    OUT.write_text(json.dumps(strip_links(credits), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{OUT}: {len(credits.get('supporters', []))} supporters, {len(credits.get('contributors', []))} contributors")


if __name__ == "__main__":
    main()

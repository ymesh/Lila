"""Triage github.com/lila-3d/issues: dump issues to Markdown and JSON, reply, label, close, fetch private attachments.

    python scripts/issues.py [--out DIR] [--since YYYY-MM-DD] [--unsolved] [--all]
    python scripts/issues.py --solve N --note "Fixed: ..."
    python scripts/issues.py --dismiss N --note "Works as intended: ..."
    python scripts/issues.py --ask N --note "Which menu was open?"
    python scripts/issues.py --hold N [--note "..."]
    python scripts/issues.py --unsolve N
    python scripts/issues.py --retag N Bug
    python scripts/issues.py --unofficial N
    python scripts/issues.py --reply N "Which menu was open?"
    python scripts/issues.py --files N
    python scripts/issues.py --setup

GitHub calls go through the gh CLI as the Lila Reports app, so replies, labels and closes show as lila-reports[bot]:
LILA_REPORTS_APP_KEY is the path to the app's private key (signed with node). --files reads the support bundle and
screenshot from the telemetry Worker with LILA_CREDITS_ADMIN_TOKEN (its ADMIN_TOKEN secret) and unzips the bundle.
Each dump labels open issues that carry no triage label "Pending" (--no-pending to skip).
Reports sent from Lila carry a lila-report marker; their reporter is the Reporter row when they gave a GitHub
user, and otherwise they read replies and answer in Help > My Reports (posted by the bot, ending in a lila-reporter
marker, which sets the issue back to Pending).
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GH_REPO = "lila-3d/issues"
WORKER = os.environ.get("LILA_ISSUES_WORKER", "https://lila-telemetry.lila3d.workers.dev")
KINDS = ["Bug", "Crash", "Performance", "Request", "Question"]
TRIAGE = ["Pending", "Needs Info", "On Hold", "Solved", "Dismissed"]
LABELS = {
    "Bug": ("d73a4a", "Something is broken"),
    "Crash": ("b60205", "Lila crashed"),
    "Request": ("a2eeef", "A feature or change"),
    "Performance": ("fbca04", "Slow or heavy"),
    "Question": ("d876e3", "How do I..."),
    "Pending": ("ededed", "Not looked at yet, or being worked on"),
    "Solved": ("0e8a16", "Fixed; out with the next update"),
    "Needs Info": ("f9d0c4", "Waiting on the reporter; their reply sets Pending"),
    "On Hold": ("c5def5", "Parked: waiting on a repro or other work"),
    "Dismissed": ("bfbfbf", "Closed without a fix"),
}


APP_ID = "5602963"
INSTALLATION_ID = "178359544"

# Signs an RS256 GitHub App JWT: key path in argv, claims JSON on stdin, token on stdout.
SIGN_JWT = r"""
const { createPrivateKey, sign } = require("node:crypto");
const fs = require("node:fs");
const b64 = (b) => Buffer.from(b).toString("base64url");
const head = b64(JSON.stringify({ alg: "RS256", typ: "JWT" }));
const body = b64(fs.readFileSync(0, "utf8"));
const sig = sign("sha256", Buffer.from(`${head}.${body}`), createPrivateKey(fs.readFileSync(process.argv[1])));
process.stdout.write(`${head}.${body}.${b64(sig)}`);
"""

_token = None

UNOFFICIAL_NOTE = ("Reports are taken from official Lila builds only, sent with Help > Report Issue so the build is "
                   "known. Download Lila from lila-3d.github.io and report from there.")


def app_token():
    """An installation token for the Lila Reports app, made once per run."""
    global _token
    if _token:
        return _token
    key = user_env("LILA_REPORTS_APP_KEY")
    if not key or not Path(key).exists():
        sys.exit("set LILA_REPORTS_APP_KEY to the Lila Reports app's private key (.pem)")
    import time
    now = int(time.time()) - 60
    claims = json.dumps({"iat": now, "exp": now + 540, "iss": APP_ID})
    try:
        jwt = subprocess.run(["node", "-e", SIGN_JWT, key], input=claims, capture_output=True, text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError) as e:
        sys.exit(f"couldn't sign the app token with node: {getattr(e, 'stderr', '') or e}")
    req = urllib.request.Request(
        f"https://api.github.com/app/installations/{INSTALLATION_ID}/access_tokens",
        method="POST",
        headers={"Authorization": f"Bearer {jwt}", "Accept": "application/vnd.github+json", "User-Agent": "lila-issues"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            _token = json.loads(r.read())["token"]
    except urllib.error.HTTPError as e:
        sys.exit(f"GitHub app token: {e.code} {e.read().decode(errors='replace')}")
    return _token


def user_env(name):
    """The variable from the process, falling back to the registry so a fresh setx works without a new terminal."""
    if os.environ.get(name):
        return os.environ[name]
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as k:
                return winreg.QueryValueEx(k, name)[0]
        except OSError:
            pass
    return None


def gh(*args, body=None):
    cmd = ["gh", "api", *args]
    if body is not None:
        cmd += ["--input", "-"]
    env = {**os.environ, "GH_TOKEN": app_token()}
    try:
        out = subprocess.run(cmd, input=json.dumps(body) if body is not None else None, capture_output=True, text=True, encoding="utf-8", env=env)
    except FileNotFoundError:
        sys.exit("gh is not installed")
    if out.returncode != 0:
        sys.exit(f"gh api {' '.join(args)}: {out.stderr.strip() or out.stdout.strip()}")
    return json.loads(out.stdout) if out.stdout.strip() else None


def gh_pages(path):
    """Every item of a paged list endpoint."""
    items, page = [], 1
    while True:
        sep = "&" if "?" in path else "?"
        batch = gh(f"{path}{sep}per_page=100&page={page}")
        items += batch
        if len(batch) < 100:
            return items
        page += 1


def issue(n):
    return gh(f"repos/{GH_REPO}/issues/{n}")


def labels_of(it):
    return [l["name"] for l in it.get("labels", [])]


def set_labels(n, labels):
    gh("-X", "PUT", f"repos/{GH_REPO}/issues/{n}/labels", body={"labels": labels})


def comment_author(c):
    """Who wrote a comment: replies sent from My Reports are posted by the bot but are the reporter's."""
    if c["user"]["login"].endswith("[bot]") and "<!-- lila-reporter -->" in (c.get("body") or ""):
        return "reporter (from Lila)"
    return c["user"]["login"]


def from_lila(it):
    return "<!-- lila-report " in (it.get("body") or "")


def reporter(it):
    """The GitHub user to mention: the author, or for Lila reports the Reporter row if they gave one."""
    if not from_lila(it):
        return it["user"]["login"]
    m = re.search(r"^\| Reporter \| @([A-Za-z0-9-]+) \|$", it.get("body") or "", re.M)
    return m.group(1) if m else None


def comment(n, text):
    it = issue(n)
    who = reporter(it)
    body = f"@{who} {text}" if who and f"@{who}" not in text else text
    gh("-X", "POST", f"repos/{GH_REPO}/issues/{n}/comments", body={"body": body})


def triage(n, label, note=None, close=None, reopen=False):
    it = issue(n)
    keep = [l for l in labels_of(it) if l not in TRIAGE]
    set_labels(n, keep + [label])
    if note:
        comment(n, note)
    if close:
        gh("-X", "PATCH", f"repos/{GH_REPO}/issues/{n}", body={"state": "closed", "state_reason": close})
    elif reopen and it["state"] == "closed":
        gh("-X", "PATCH", f"repos/{GH_REPO}/issues/{n}", body={"state": "open"})


def retag(n, kind):
    if kind not in KINDS:
        sys.exit(f"kind must be one of {', '.join(KINDS)}")
    keep = [l for l in labels_of(issue(n)) if l not in KINDS]
    set_labels(n, keep + [kind])


def setup():
    have = {l["name"] for l in gh_pages(f"repos/{GH_REPO}/labels")}
    for name, (color, desc) in LABELS.items():
        if name in have:
            gh("-X", "PATCH", f"repos/{GH_REPO}/labels/{urllib.request.quote(name)}", body={"color": color, "description": desc})
        else:
            gh("-X", "POST", f"repos/{GH_REPO}/labels", body={"name": name, "color": color, "description": desc})
    print(f"{len(LABELS)} labels on {GH_REPO}")


def worker_get(query):
    token = user_env("LILA_CREDITS_ADMIN_TOKEN")
    if not token:
        sys.exit("set LILA_CREDITS_ADMIN_TOKEN to the Worker's ADMIN_TOKEN")
    req = urllib.request.Request(f"{WORKER}/issues/file?{query}", headers={"Authorization": f"Bearer {token}", "User-Agent": "lila-issues"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        sys.exit(f"Worker: {e.code} {e.read().decode(errors='replace')}")


def files(n, out):
    names = json.loads(worker_get(f"number={n}"))["files"]
    if not names:
        print(f"#{n} has no attachments")
        return
    dest = out / "files" / str(n)
    dest.mkdir(parents=True, exist_ok=True)
    for name in names:
        path = dest / name
        path.write_bytes(worker_get(f"number={n}&name={name}"))
        print(path)
        if name.endswith(".zip"):
            with zipfile.ZipFile(path) as z:
                z.extractall(dest / path.stem)
            print(dest / path.stem)


def status(it):
    for t in ["Solved", "Dismissed", "Needs Info", "On Hold", "Pending"]:
        if t in labels_of(it):
            return t
    return it["state"]


def dump(args):
    since = f"&since={args.since}T00:00:00Z" if args.since else ""
    state = "all" if args.all or args.since else "open"
    items = [it for it in gh_pages(f"repos/{GH_REPO}/issues?state={state}{since}") if "pull_request" not in it]
    if args.since:
        items = [it for it in items if it["created_at"][:10] >= args.since]
    out = []
    for it in items:
        if not args.no_pending and it["state"] == "open" and not set(labels_of(it)) & set(TRIAGE):
            set_labels(it["number"], labels_of(it) + ["Pending"])
            it["labels"].append({"name": "Pending"})
        if args.unsolved and status(it) in ("Solved", "Dismissed"):
            continue
        comments = gh_pages(f"repos/{GH_REPO}/issues/{it['number']}/comments") if it["comments"] else []
        out.append({
            "number": it["number"],
            "title": it["title"],
            "url": it["html_url"],
            "state": it["state"],
            "status": status(it),
            "labels": labels_of(it),
            "kind": next((l for l in labels_of(it) if l in KINDS), None),
            "author": it["user"]["login"],
            "reporter": reporter(it),
            "from_lila": from_lila(it),
            "created": it["created_at"],
            "body": it.get("body") or "",
            "comments": [{"author": comment_author(c), "role": c["author_association"], "at": c["created_at"], "body": c["body"]} for c in comments],
        })
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "issues.json").write_text(json.dumps(out, indent=2), encoding="utf-8")
    lines = [f"# {GH_REPO}: {len(out)} issues", ""]
    for i in out:
        tags = ", ".join(i["labels"]) or "none"
        lines += [f"## #{i['number']} {i['title']}", "", f"{i['url']} | {i['status']} | labels: {tags} | by {i['reporter'] or 'Lila user (no GitHub)'} | {i['created'][:10]}", ""]
        if not i["kind"]:
            lines += ["**Untagged:** give it a kind with --retag.", ""]
        if not i["from_lila"] and i["state"] == "open":
            lines += ["**Not sent from Lila:** no verified build; close it with --unofficial.", ""]
        lines += [i["body"], ""]
        for c in i["comments"]:
            lines += [f"> **{c['author']}** ({c['role'].lower()}, {c['at'][:10]})", ">", *[f"> {l}" for l in c["body"].splitlines()], ""]
    (args.out / "issues.md").write_text("\n".join(lines), encoding="utf-8")
    untagged = sum(1 for i in out if not i["kind"])
    print(f"{len(out)} issues ({untagged} untagged) -> {args.out / 'issues.md'}")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--out", type=Path, default=REPO / "build" / "issues")
    p.add_argument("--since", help="issues created on or after this date (YYYY-MM-DD), open or closed")
    p.add_argument("--all", action="store_true", help="closed issues too")
    p.add_argument("--unsolved", action="store_true", help="skip issues labelled Solved or Dismissed")
    p.add_argument("--no-pending", action="store_true", help="don't label untriaged open issues Pending")
    p.add_argument("--solve", type=int, metavar="N", help="label Solved, reply with --note and close")
    p.add_argument("--dismiss", type=int, metavar="N", help="label Dismissed, reply with --note and close as not planned")
    p.add_argument("--ask", type=int, metavar="N", help="label Needs Info and ask the reporter --note; their reply sets Pending")
    p.add_argument("--hold", type=int, metavar="N", help="label On Hold: parked, waiting on a repro or other work")
    p.add_argument("--unsolve", type=int, metavar="N", help="back to Pending, reopened")
    p.add_argument("--retag", nargs=2, metavar=("N", "KIND"), help="replace the kind label (Bug, Crash, Performance, Request, Question)")
    p.add_argument("--reply", nargs=2, metavar=("N", "TEXT"), help="comment, mentioning the reporter, without changing labels")
    p.add_argument("--unofficial", type=int, metavar="N", help="dismiss an issue not sent from an official build, with the stock reply")
    p.add_argument("--note", help="the reply that goes with --solve, --dismiss, --ask, --hold or --unsolve")
    p.add_argument("--files", type=int, metavar="N", help="download the private attachments to OUT/files/N")
    p.add_argument("--setup", action="store_true", help="create or update the labels")
    args = p.parse_args()

    if args.setup:
        setup()
    elif args.solve:
        if not args.note:
            sys.exit("--solve needs --note with the cause and fix")
        triage(args.solve, "Solved", args.note, close="completed")
    elif args.dismiss:
        if not args.note:
            sys.exit("--dismiss needs --note saying why")
        triage(args.dismiss, "Dismissed", args.note, close="not_planned")
    elif args.ask:
        if not args.note:
            sys.exit("--ask needs --note with the question")
        triage(args.ask, "Needs Info", args.note)
    elif args.hold:
        triage(args.hold, "On Hold", args.note)
    elif args.unsolve:
        triage(args.unsolve, "Pending", args.note, reopen=True)
    elif args.unofficial:
        triage(args.unofficial, "Dismissed", args.note or UNOFFICIAL_NOTE, close="not_planned")
    elif args.retag:
        retag(int(args.retag[0]), args.retag[1])
    elif args.reply:
        comment(int(args.reply[0]), args.reply[1])
    elif args.files:
        files(args.files, args.out)
    else:
        dump(args)


if __name__ == "__main__":
    main()

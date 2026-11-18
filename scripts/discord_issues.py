"""Dump the Lila Dev #issues forum channel (open and archived posts) to Markdown and JSON.

    python scripts/discord_issues.py [--channel ID] [--out DIR] [--since YYYY-MM-DD] [--no-attachments] [--unsolved]
    python scripts/discord_issues.py --solve POST_ID [--note "Fixed in abc123"]
    python scripts/discord_issues.py --unsolve POST_ID
    python scripts/discord_issues.py --dismiss POST_ID --note "Works as intended: ..."
    python scripts/discord_issues.py --hold POST_ID [--note "..."]
    python scripts/discord_issues.py --retag POST_ID Bug
    python scripts/discord_issues.py --reply POST_ID "Which menu was open?"
    python scripts/discord_issues.py --ack POST_ID

Reads the bot token from LILA_DISCORD_TOKEN and the channel from --channel or LILA_DISCORD_ISSUES_CHANNEL.
The bot needs View Channels and Read Message History, plus the Message Content intent.
Solving needs Manage Threads, a "Solved" tag on the forum, and Send Messages in Threads for --note.
Each dump tags open posts that carry neither tag with "Pending" (--no-pending to skip); --solve swaps Pending for Solved, --unsolve swaps back.
A post whose last message isn't from the bot or a maintainer (user ids in LILA_DISCORD_MAINTAINERS, comma separated) needs a reply:
it is listed at the end and marked in issues.md, whatever its tags, even with --unsolved. --ack marks the last message seen
(the bot reacts to it) when it needs no answer, such as a thank-you.
Writes issues.md, issues.json and attachments/<post id>/ to build/discord by default.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
API = "https://discord.com/api/v10"
USER_AGENT = "DiscordBot (https://github.com/lila-3d/Lila, 1.0)"


def user_env(name):
    """The variable from the process, falling back to the registry so a fresh setx works without a new terminal."""
    if os.environ.get(name):
        return os.environ[name]
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as key:
                return winreg.QueryValueEx(key, name)[0]
        except OSError:
            pass
    return None


class Discord:
    def __init__(self, token):
        self.token = token

    def get(self, path):
        return self.request("GET", path)

    def request(self, method, path, body=None):
        headers = {"Authorization": f"Bot {self.token}", "User-Agent": USER_AGENT}
        data = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(API + path, data=data, headers=headers, method=method)
        while True:
            try:
                with urllib.request.urlopen(req, timeout=30) as resp:
                    raw = resp.read()
                    return json.loads(raw) if raw else None
            except urllib.error.HTTPError as e:
                if e.code == 429:
                    time.sleep(float(json.load(e).get("retry_after", 1.0)) + 0.1)
                    continue
                body = e.read().decode("utf-8", "replace")
                hint = {401: "token rejected", 403: "bot lacks a permission for this", 404: "unknown channel or bot not in the server"}.get(e.code, "")
                sys.exit(f"{method} {path} -> {e.code} {hint}\n{body}")


def download(url, dest):
    if dest.exists():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as resp:
        dest.write_bytes(resp.read())


def forum_posts(api, channel):
    info = api.get(f"/channels/{channel}")
    posts = [t for t in api.get(f"/guilds/{info['guild_id']}/threads/active")["threads"] if t.get("parent_id") == channel]
    before = None
    while True:
        query = "?limit=100" + (f"&before={before}" if before else "")
        page = api.get(f"/channels/{channel}/threads/archived/public{query}")
        posts += page["threads"]
        if not page.get("has_more") or not page["threads"]:
            break
        before = page["threads"][-1]["thread_metadata"]["archive_timestamp"]
    return info, posts


def thread_messages(api, thread_id):
    messages, before = [], None
    while True:
        page = api.get(f"/channels/{thread_id}/messages?limit=100" + (f"&before={before}" if before else ""))
        messages += page
        if len(page) < 100:
            break
        before = page[-1]["id"]
    return sorted(messages, key=lambda m: int(m["id"]))


HOLD_TAG = "On Hold"


def maintainers(api):
    """The bot plus the user ids in LILA_DISCORD_MAINTAINERS: their messages count as answers."""
    ids = {api.get("/users/@me")["id"]}
    ids |= {i.strip() for i in (user_env("LILA_DISCORD_MAINTAINERS") or "").split(",") if i.strip()}
    return ids


ACK = "%F0%9F%91%80"  # eyes emoji, URL-encoded for the reactions endpoint


def acked(message):
    return any(r.get("me") and r["emoji"].get("name") == "👀" for r in message.get("reactions", []))


def ack(api, post_id):
    """Reacts to the post's last message so the dump stops listing it as needing a reply."""
    last = thread_messages(api, post_id)[-1]
    api.request("PUT", f"/channels/{post_id}/messages/{last['id']}/reactions/{ACK}/@me")
    print(f"acked: {last['author']['username']}: {last.get('content', '')[:60]}")


def find_tag(forum, name):
    return next((t["id"] for t in forum.get("available_tags", []) if t["name"].lower() == name.lower()), None)


def retag(api, post_id, category, status_names):
    """Replaces a post's category tags (everything but the status tags) with one category."""
    post = api.get(f"/channels/{post_id}")
    forum = api.get(f"/channels/{post['parent_id']}")
    new_tag = find_tag(forum, category)
    if not new_tag:
        names = ", ".join(t["name"] for t in forum.get("available_tags", []))
        sys.exit(f"#{forum['name']} has no '{category}' tag; it has: {names}")
    status = {find_tag(forum, n) for n in status_names} - {None}
    tags = [t for t in post.get("applied_tags", []) if t in status] + [new_tag]
    # Archived posts reject edits, so reopen first.
    api.request("PATCH", f"/channels/{post_id}", {"archived": False, "applied_tags": tags})
    print(f"{category}: {post['name']}")


def set_solved(api, post_id, solved, solved_name, pending_name, note, dismissed_name="Dismissed"):
    """Swaps a post between the pending tag and solved_name (Solved, Dismissed or On Hold), optionally replying with a note."""
    post = api.get(f"/channels/{post_id}")
    forum = api.get(f"/channels/{post['parent_id']}")
    solved_tag, pending_tag = find_tag(forum, solved_name), find_tag(forum, pending_name)
    if not solved_tag:
        sys.exit(f"#{forum['name']} has no '{solved_name}' tag; add it in the channel's Tags settings")
    states = {solved_tag, pending_tag, find_tag(forum, dismissed_name), find_tag(forum, "Solved"), find_tag(forum, HOLD_TAG)} - {None}
    tags = [t for t in post.get("applied_tags", []) if t not in states]
    add = solved_tag if solved else pending_tag
    if add:
        if len(tags) >= 5:
            sys.exit("post already has 5 tags, Discord's maximum")
        tags.append(add)
    # Archived posts reject edits, so reopen first.
    api.request("PATCH", f"/channels/{post_id}", {"archived": False, "applied_tags": tags})
    if note:
        post_message(api, post, note)
    print(f"{solved_name.lower() if solved else 'pending'}: {post['name']}")


def post_message(api, post, text):
    """Posts text in the post's thread, mentioning its author so Discord notifies them."""
    author = post.get("owner_id")
    content = f"<@{author}> {text}" if author else text
    api.request("POST", f"/channels/{post['id']}/messages", {"content": content, "allowed_mentions": {"users": [author] if author else []}})


def reply(api, post_id, text):
    """Posts text in the post's thread, leaving its tags alone."""
    post = api.get(f"/channels/{post_id}")
    # Archived posts reject messages, so reopen first.
    if post.get("thread_metadata", {}).get("archived"):
        api.request("PATCH", f"/channels/{post_id}", {"archived": False})
    post_message(api, post, text)
    print(f"replied: {post['name']}")


def edit(api, post_id, message_id, text):
    """Replaces the text of one of the bot's own messages in a post. No new ping: the reporter was notified when it was posted."""
    api.request("PATCH", f"/channels/{post_id}/messages/{message_id}", {"content": text, "allowed_mentions": {"parse": []}})
    print(f"edited: {message_id}")


def created_at(snowflake):
    return datetime.fromtimestamp(((int(snowflake) >> 22) + 1420070400000) / 1000, tz=timezone.utc)


def to_markdown(channel_name, records):
    lines = [f"# #{channel_name}: {len(records)} posts", ""]
    for r in records:
        tags = ", ".join(r["tags"]) or "untagged"
        state = ("archived" if r["archived"] else "open") + (", needs reply" if r["needs_reply"] else "")
        lines += [f"## {r['title']}", "", f"[{tags}] {state}, {r['created']}, id {r['id']}", ""]
        for m in r["messages"]:
            lines.append(f"**{m['author']}** ({m['time']}): {m['content']}")
            lines += [f"- attachment: {a['local'] or a['url']}" for a in m["attachments"]]
            lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--channel", default=user_env("LILA_DISCORD_ISSUES_CHANNEL"))
    parser.add_argument("--out", type=Path, default=REPO / "build" / "discord")
    parser.add_argument("--since", help="only posts created on or after this date (YYYY-MM-DD)")
    parser.add_argument("--no-attachments", action="store_true", help="link attachments instead of downloading them")
    parser.add_argument("--unsolved", action="store_true", help="skip posts tagged Solved or Dismissed")
    parser.add_argument("--retag", nargs=2, metavar=("POST_ID", "TAG"), help="replace a post's category tag (Bug, Request, ...), keeping Pending/Solved")
    parser.add_argument("--solve", metavar="POST_ID", help="add the solved tag to a post")
    parser.add_argument("--dismiss", metavar="POST_ID", help="tag a post Dismissed: closed without a fix")
    parser.add_argument("--hold", metavar="POST_ID", help="tag a post On Hold: open but waiting (no repro, waiting on the reporter); --unsolve returns it to Pending")
    parser.add_argument("--unsolve", metavar="POST_ID", help="remove the solved tag from a post")
    parser.add_argument("--reply", nargs=2, metavar=("POST_ID", "TEXT"), help="reply in a post, mentioning its author, without changing its tags")
    parser.add_argument("--edit", nargs=3, metavar=("POST_ID", "MESSAGE_ID", "TEXT"), help="replace the text of one of the bot's messages")
    parser.add_argument("--ack", metavar="POST_ID", help="mark the post's last message seen (needs no reply)")
    parser.add_argument("--note", help="with --solve/--unsolve, also reply in the post with this text, mentioning its author")
    parser.add_argument("--solved-tag", default="Solved", help="name of the forum tag that marks a post solved")
    parser.add_argument("--pending-tag", default="Pending", help="tag given to open posts that are neither pending nor solved")
    parser.add_argument("--no-pending", action="store_true", help="don't tag new posts as pending while dumping")
    args = parser.parse_args()

    token = user_env("LILA_DISCORD_TOKEN")
    if not token:
        sys.exit("LILA_DISCORD_TOKEN is not set")
    api = Discord(token)
    if args.retag:
        retag(api, args.retag[0], args.retag[1], (args.solved_tag, args.pending_tag, "Dismissed", HOLD_TAG))
        return
    if args.reply:
        reply(api, args.reply[0], args.reply[1])
        return
    if args.ack:
        ack(api, args.ack)
        return
    if args.edit:
        edit(api, *args.edit)
        return
    if (args.solve or args.dismiss) and not args.note:
        sys.exit("--solve and --dismiss need --note: tell the reporter the outcome")
    if args.dismiss:
        set_solved(api, args.dismiss, True, "Dismissed", args.pending_tag, args.note)
        return
    if args.hold:
        set_solved(api, args.hold, True, HOLD_TAG, args.pending_tag, args.note)
        return
    if args.solve or args.unsolve:
        set_solved(api, args.solve or args.unsolve, bool(args.solve), args.solved_tag, args.pending_tag, args.note)
        return
    if not args.channel:
        sys.exit("pass --channel or set LILA_DISCORD_ISSUES_CHANNEL")

    info, posts = forum_posts(api, args.channel)
    tag_names = {t["id"]: t["name"] for t in info.get("available_tags", [])}
    solved_tag, pending_tag = find_tag(info, args.solved_tag), find_tag(info, args.pending_tag)
    dismissed_tag = find_tag(info, "Dismissed")
    hold_tag = find_tag(info, HOLD_TAG)
    if pending_tag and not args.no_pending:
        for post in posts:
            applied = post.get("applied_tags", [])
            # Archived posts are skipped: tagging one would reopen it.
            if post.get("thread_metadata", {}).get("archived") or {pending_tag, solved_tag, dismissed_tag, hold_tag} & set(applied) - {None} or len(applied) >= 5:
                continue
            post["applied_tags"] = applied + [pending_tag]
            api.request("PATCH", f"/channels/{post['id']}", {"applied_tags": post["applied_tags"]})
            print(f"pending: {post['name']}")
    since = datetime.fromisoformat(args.since).replace(tzinfo=timezone.utc) if args.since else None

    staff = maintainers(api)
    records = []
    for post in sorted(posts, key=lambda p: int(p["id"]), reverse=True):
        created = created_at(post["id"])
        if since and created < since:
            continue
        raw = thread_messages(api, post["id"])
        needs_reply = bool(raw) and raw[-1]["author"]["id"] not in staff and not acked(raw[-1])
        tags = [tag_names.get(t, t) for t in post.get("applied_tags", [])]
        if args.unsolved and not needs_reply and {args.solved_tag.lower(), "dismissed"} & {t.lower() for t in tags}:
            continue
        messages = []
        for m in raw:
            attachments = []
            for a in m.get("attachments", []):
                local = None
                if not args.no_attachments:
                    dest = args.out / "attachments" / post["id"] / f"{a['id']}_{a['filename']}"
                    download(a["url"], dest)
                    local = str(dest)
                attachments.append({"filename": a["filename"], "url": a["url"], "local": local})
            messages.append({
                "id": m["id"],
                "author": m["author"].get("global_name") or m["author"]["username"],
                "time": created_at(m["id"]).strftime("%Y-%m-%d %H:%M"),
                "content": m.get("content", ""),
                "attachments": attachments,
            })
        records.append({
            "id": post["id"],
            "title": post["name"],
            "tags": tags,
            "archived": post.get("thread_metadata", {}).get("archived", False),
            "created": created.strftime("%Y-%m-%d %H:%M"),
            "needs_reply": needs_reply,
            "messages": messages,
        })

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "issues.json").write_text(json.dumps(records, indent=2, ensure_ascii=False), encoding="utf-8")
    (args.out / "issues.md").write_text(to_markdown(info.get("name", "issues"), records), encoding="utf-8")
    print(f"{len(records)} posts -> {args.out / 'issues.md'}")
    for r in records:
        if r["needs_reply"]:
            print(f"needs reply: {r['title']} ({r['id']})")


if __name__ == "__main__":
    main()

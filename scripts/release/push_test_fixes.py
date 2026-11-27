"""Push main to origin when every unpushed commit is a plain commit by the repo's own author.

The one push an agent may run itself (the global hook allows exactly
`python -I scripts/release/push_test_fixes.py`): the fixes a failed release gate needs, so the release can resume.
Anything else waits for a person to push.
"""

import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# Commits must look entirely human-authored.
FORBIDDEN = re.compile(r"co-authored-by|claude|anthropic|\bai\b|generated with", re.IGNORECASE)


def git(*args):
    return subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()


def refuse(reason):
    print(f"refused: {reason}")
    sys.exit(1)


def main():
    if git("rev-parse", "--abbrev-ref", "HEAD") != "main":
        refuse("not on main")
    git("fetch", "--quiet", "origin", "main")
    if subprocess.run(["git", "merge-base", "--is-ancestor", "origin/main", "HEAD"], cwd=REPO).returncode != 0:
        refuse("origin/main is not an ancestor of HEAD, so this would not be a fast-forward")
    commits = git("rev-list", "--reverse", "origin/main..HEAD").split()
    if not commits:
        refuse("nothing to push")

    email = git("config", "user.email")
    for c in commits:
        short = c[:8]
        parents = git("rev-list", "--parents", "-n", "1", c).split()[1:]
        if len(parents) != 1:
            refuse(f"{short} is a merge")
        author, committer = git("show", "-s", "--format=%ae%n%ce", c).splitlines()
        if author != email or committer != email:
            refuse(f"{short} is authored or committed by {author} / {committer}, not {email}")
        message = git("show", "-s", "--format=%B", c)
        if FORBIDDEN.search(message):
            refuse(f"{short} has a forbidden line in its message: {FORBIDDEN.search(message).group(0)!r}")

    for c in commits:
        print(git("show", "-s", "--format=%h %s", c))
    result = subprocess.run(["git", "push", "origin", "main"], cwd=REPO)
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()

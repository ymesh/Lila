"""File lists and patch staging for the installer (called by build.ps1).

  patches.py files <content dir> <version>
      Writes <content dir>/lila-files.json: every installed file with its size and SHA-256, which the updater checks
      before applying a patch and after an update.
  patches.py stage <content dir> <new list> <old list> <patch dir> <deletes .iss>
      Copies the files that changed since the old list into <patch dir> and writes the [InstallDelete] lines for files
      the new version dropped. Prints the number of changed and deleted files.
"""
import hashlib
import json
import os
import shutil
import sys

MANIFEST = "lila-files.json"


def excluded(rel):
    """Files Lila rewrites in its own install (bytecode, regenerated Python stubs), which can't be checked."""
    parts = rel.split("/")
    return (rel == MANIFEST or "__pycache__" in parts or rel.endswith((".pyc", ".pyi"))
            or rel.startswith("python/lila/_namespace/"))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def files(content, version):
    listed = {}
    for root, _, names in os.walk(content):
        for name in names:
            path = os.path.join(root, name)
            rel = os.path.relpath(path, content).replace("\\", "/")
            if not excluded(rel):
                listed[rel] = {"size": os.path.getsize(path), "sha256": sha256(path)}
    with open(os.path.join(content, MANIFEST), "w", encoding="utf-8") as f:
        json.dump({"version": version, "files": dict(sorted(listed.items()))}, f, indent=1)
    print(f"{len(listed)} files listed")


def stage(content, new_list, old_list, patch_dir, deletes_iss):
    new = json.load(open(new_list, encoding="utf-8"))["files"]
    old = json.load(open(old_list, encoding="utf-8"))["files"]
    changed = [rel for rel, entry in new.items() if old.get(rel, {}).get("sha256") != entry["sha256"]]
    deleted = sorted(rel for rel in old if rel not in new)
    shutil.rmtree(patch_dir, ignore_errors=True)
    # Unlisted files (Python stubs) can't be compared, and are small: every patch carries them.
    unlisted = [os.path.relpath(os.path.join(root, n), content).replace("\\", "/")
                for root, _, names in os.walk(content) for n in names]
    unlisted = [rel for rel in unlisted if excluded(rel) and rel != MANIFEST]
    for rel in changed + unlisted + [MANIFEST]:
        dest = os.path.join(patch_dir, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy2(os.path.join(content, rel), dest)
    with open(deletes_iss, "w", encoding="utf-8") as f:
        for rel in deleted:
            f.write(f'Type: files; Name: "{{app}}\\{rel.replace("/", chr(92))}"\n')
    size = sum(new[rel]["size"] for rel in changed)
    print(f"{len(changed)} changed ({size / 1048576:.0f} MB), {len(deleted)} deleted")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "files":
        files(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 7 and sys.argv[1] == "stage":
        stage(*sys.argv[2:])
    else:
        sys.exit(__doc__)

#!/usr/bin/env python3
"""Checks the repository for third-party brand and model names without storing them.

The project rule (CLAUDE.md) forbids third-party brand names in code, UI, presets and docs.
A plain-text deny-list would itself put those names into the repository, so the list is kept
as SHA-256 hashes of normalised names (scripts/preflight/names.sha256). This script hashes every
word and every run of two or three consecutive words of each file and reports a match.

Normalisation: lower case; every run of characters other than a-z and 0-9 becomes one space.
So "Foo-Bar 9" is stored and matched as "foo bar 9".

Usage:
  name-check.py --check              file names are read NUL-separated from stdin
  name-check.py --hash "Some Name"   print the hash line to add to names.sha256
"""
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = os.path.join(HERE, "names.sha256")
WORD = re.compile(r"[a-z0-9]+")
MAX_WORDS = 3


def normalise(text):
    return " ".join(WORD.findall(text.lower()))


def digest(name):
    return hashlib.sha256(name.encode("utf-8")).hexdigest()


def load():
    hashes = set()
    with open(LIST, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                hashes.add(line)
    return hashes


def check():
    hashes = load()
    if not hashes:
        print("name-check: the hash list is empty", file=sys.stderr)
        return 2
    files = [p for p in sys.stdin.buffer.read().decode("utf-8").split("\0") if p]
    seen = {}  # n-gram -> bool, so each distinct phrase is hashed once
    hits = []
    for path in files:
        try:
            with open(path, "rb") as f:
                raw = f.read()
        except OSError:
            continue
        if b"\0" in raw:
            continue  # binary
        for number, line in enumerate(raw.decode("utf-8", "replace").splitlines(), 1):
            words = WORD.findall(line.lower())
            for size in range(1, MAX_WORDS + 1):
                for i in range(len(words) - size + 1):
                    phrase = " ".join(words[i:i + size])
                    known = seen.get(phrase)
                    if known is None:
                        known = digest(phrase) in hashes
                        seen[phrase] = known
                    if known:
                        hits.append(f"{path}:{number}: listed name (hash {digest(phrase)[:12]})")
    for hit in hits[:20]:
        print(hit, file=sys.stderr)
    if hits:
        print(f"name-check: {len(hits)} match(es) in {len(files)} files", file=sys.stderr)
        return 1
    print(f"name-check: no listed names in {len(files)} files ({len(hashes)} hashes)")
    return 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--check":
        return check()
    if len(sys.argv) == 3 and sys.argv[1] == "--hash":
        name = normalise(sys.argv[2])
        if not name or len(name.split()) > MAX_WORDS:
            print(f"name-check: a name must have 1 to {MAX_WORDS} words", file=sys.stderr)
            return 2
        print(digest(name))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())

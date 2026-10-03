#!/usr/bin/env python3
"""Idempotently sync a game project's CMakeLists.txt with the scaffold template.

The managed block lives in tools/new_project_layout/templates/CMakeLists.txt.in
between `# >>> psxrecomp:<name>` / `# <<< psxrecomp:<name>` markers (single
source of truth). For each block this inserts it after the project's
psxrecomp_add_game_runtime(...) call, or replaces it in place if the markers are
already present. Run via tools/update_cmake.sh or tools/update_cmake.ps1.

  update_cmake.py [--project DIR] [--check] [--dry-run]
    --check    exit 1 if the project is out of date (writes nothing; for CI)
    --dry-run  print the unified diff, write nothing
"""
import argparse, difflib, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "tools/new_project_layout/templates/CMakeLists.txt.in"
BLOCK_RE = re.compile(
    r"^# >>> psxrecomp:(?P<n>[\w-]+).*?\n.*?^# <<< psxrecomp:(?P=n)[ \t]*\n",
    re.M | re.S)


def template_blocks():
    return {m["n"]: m.group(0) for m in BLOCK_RE.finditer(TEMPLATE.read_text(encoding="utf-8"))}


def add_runtime_end(text):
    """Index just past the closing ')' of psxrecomp_add_game_runtime(...)."""
    m = re.search(r"^\s*psxrecomp_add_game_runtime\s*\(", text, re.M)
    if not m:
        return None
    depth, i, in_str = 0, m.end() - 1, False
    while i < len(text):
        c = text[i]
        if c == '"' and text[i - 1] != "\\":
            in_str = not in_str
        elif not in_str:
            if c == "#":
                i = text.find("\n", i)
                if i < 0:
                    return None
                continue
            depth += c == "("
            depth -= c == ")"
            if depth == 0:
                nl = text.find("\n", i)
                return len(text) if nl < 0 else nl + 1
        i += 1
    return None


def sync(text):
    for name, block in template_blocks().items():
        m = re.search(r"^# >>> psxrecomp:%s.*?\n.*?^# <<< psxrecomp:%s[ \t]*\n" % (re.escape(name), re.escape(name)), text, re.M | re.S)
        if m:
            text = text[:m.start()] + block + text[m.end():]
            continue
        end = add_runtime_end(text)
        if end is None:
            sys.exit("update_cmake: no psxrecomp_add_game_runtime(...) call found")
        text = text[:end] + "\n" + block + text[end:]
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", default=".", help="game project dir (default: cwd)")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    path = Path(a.project).resolve() / "CMakeLists.txt"
    if not path.is_file():
        sys.exit(f"update_cmake: {path} not found")
    old = path.read_text(encoding="utf-8")
    new = sync(old)
    if new == old:
        print("update_cmake: up to date")
        return 0
    if a.check or a.dry_run:
        sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), new.splitlines(True), "CMakeLists.txt", "CMakeLists.txt (updated)"))
        return 1 if a.check else 0
    path.write_text(new, encoding="utf-8", newline="")
    print(f"update_cmake: updated {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

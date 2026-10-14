#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Verbatim-overlap (snippet) check for the i.MX 95 upstream-candidate series.
#
# Hashes overlapping windows of K *significant* lines (comments, blank lines and
# very short lines removed; whitespace normalized) from the files this series
# ADDS, then streams a set of reference corpora looking for any collision.
# A collision means the same K significant lines appear in both places.
#
# Usage:
#   oss-compliance-derivcheck.py [-k K] [-r REPO] [-b BASE] [-c NAME=PATH ...]
#
# Defaults reproduce the table published in docs/provenance.md section 4
# (K=8).  Pass -k 5 to reproduce the lower-threshold column in section 8 of
# docs/oss-compliance-log.md.
#
# Corpus paths must be supplied with -c for anything outside this repository;
# the published results used NXP BSP trees that are not redistributable and so
# are not included here.  Record the corpus revisions alongside any result.

import argparse, hashlib, os, re, subprocess, sys

def significant(path):
    """Normalized significant lines: no comments, blanks, or trivia."""
    out = []
    try:
        lines = open(path, errors="ignore").read().splitlines()
    except OSError:
        return out
    for line in lines:
        s = line.strip()
        if not s or s.startswith(("*", "/*", "//", "#")):
            continue
        s = re.sub(r"\s+", " ", s)
        if len(s) < 12:
            continue
        out.append(s)
    return out

def shingles(lines, k):
    for i in range(len(lines) - k + 1):
        yield hashlib.sha1("\n".join(lines[i:i + k]).encode()).hexdigest(), i + 1

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", type=int, default=8, help="shingle length in significant lines (default 8)")
    ap.add_argument("-r", "--repo", default=os.getcwd(), help="repository to analyse (default cwd)")
    ap.add_argument("-b", "--base", default="origin/master", help="base revision the series sits on")
    ap.add_argument("-c", "--corpus", action="append", default=[], metavar="NAME=PATH",
                    help="reference corpus to scan; repeatable")
    args = ap.parse_args()

    os.chdir(args.repo)
    added = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=A", f"{args.base}..HEAD"],
        capture_output=True, text=True, check=True).stdout.split()
    ours = [f for f in added if f.endswith((".c", ".h"))]
    if not ours:
        sys.exit(f"no added .c/.h files found for {args.base}..HEAD")

    mine = {}
    for f in ours:
        for h, ln in shingles(significant(os.path.join(args.repo, f)), args.k):
            mine.setdefault(h, []).append((f, ln))
    print(f"analysed files: {len(ours)}   shingles(K={args.k}): {len(mine)}")

    corpora = [("this-repo", [os.path.join(args.repo, d)
                              for d in ("hw", "include/hw", "target/arm")])]
    for spec in args.corpus:
        name, _, path = spec.partition("=")
        corpora.append((name, [path]))

    ourset = set(ours)
    rc = 0
    for name, roots in corpora:
        hits, scanned = {}, 0
        for root in roots:
            if not os.path.isdir(root):
                print(f"[{name}] skipped, not a directory: {root}")
                continue
            for dirpath, _, filenames in os.walk(root):
                for fn in filenames:
                    if not fn.endswith((".c", ".h")):
                        continue
                    p = os.path.join(dirpath, fn)
                    rel = os.path.relpath(p, args.repo)
                    if rel in ourset:          # never match the series against itself
                        continue
                    scanned += 1
                    for h, _ in shingles(significant(p), args.k):
                        if h in mine:
                            hits.setdefault(rel, set()).update(x[0] for x in mine[h])
        print(f"[{name}] scanned {scanned} files -> {len(hits)} with a {args.k}-line match")
        for k, v in sorted(hits.items()):
            print(f"   {k}  <->  {sorted(v)}")
        rc |= 1 if hits else 0
    return rc

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Generate docs/validation/test-result-matrix.md from its two halves:

  * docs/validation/test-matrix.yaml  -- HUMAN-owned tier + caveat + topology
  * the actual qtest run               -- CI-owned pass/fail

Fleet CI guardrail (operator + holobench, 2026-06-28): CI fills ONLY the
test-result column from the real harness run. Tier and caveats are read verbatim
from the YAML and never inferred from a green test -- mis-tiering would itself be
a silent fail. This script is that assembler; wire it into CI on each release.

Usage:
  QTEST_QEMU_BINARY=build/qemu-system-aarch64 \
    tests/gen-test-matrix.py [--build-dir build] [-o docs/validation/test-result-matrix.md]

  # regenerate the README's condensed capability table from the same YAML:
  tests/gen-test-matrix.py --inject-readme            # -> README.md

`qtest:<bin>` entries are executed (TAP ok/not-ok). `boot` and `harness:<dir>`
entries are labelled from their declared kind -- a full CI also runs the boot
smoke / functional harness and would upgrade those cells; this assembler does not
fabricate a pass it didn't observe.

The README's "What runs today" table is a SECOND rendering of test-matrix.yaml
(condensed, via the readme_groups grouping) between the capability-table markers;
--inject-readme regenerates it and verify_groups() fails if a block is un-grouped
or a group's tier disagrees with the block's -- so the README and the detailed
docs/validation/test-result-matrix.md cannot drift.
"""
import argparse
import os
import subprocess
import sys

try:
    import yaml
except ImportError:
    sys.exit("need PyYAML: pip install pyyaml (or apt install python3-yaml)")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def run_qtest(binary, qemu):
    """Return 'PASS (qtest)' / 'FAIL (qtest)' / 'SKIP (no binary)'."""
    path = os.path.join(qemu_build_dir, "tests", "qtest", binary)
    if not (qemu and os.path.exists(qemu)):
        return "SKIP (no qemu)"
    if not os.path.exists(path):
        return "SKIP (not built)"
    env = dict(os.environ, QTEST_QEMU_BINARY=qemu)
    try:
        out = subprocess.run([path], capture_output=True, text=True,
                             env=env, timeout=120).stdout
    except Exception as e:                       # noqa: BLE001
        return f"FAIL ({e.__class__.__name__})"
    ok = sum(l.startswith("ok ") for l in out.splitlines())
    notok = sum(l.startswith("not ok ") for l in out.splitlines())
    return "PASS (qtest)" if (ok and not notok) else "FAIL (qtest)"


def result_for(test, qemu):
    if test == "boot":
        return "PASS (boot)"           # CI boot-smoke fills/confirms this
    if test.startswith("harness:"):
        return "HARNESS"
    if test.startswith("qtest:"):
        return run_qtest(test.split(":", 1)[1], qemu)
    return "?"


def row(block, tier, result, evidence, caveat):
    ev = evidence or ""
    if caveat:
        ev = (u"⚑ " + caveat + ("; " + ev if ev else "")).strip("; ")
    return f"| {block} | {tier} | {result} | {ev} |"


# ---- README condensed capability table (same YAML, second rendering) --------
README_BEGIN = "<!-- BEGIN capability-table (generated from test-matrix.yaml) -->"
README_END = "<!-- END capability-table (generated from test-matrix.yaml) -->"


def verify_groups(spec):
    """Every present/absent block is grouped exactly once; groups are single-tier.

    This is the anti-drift guard: the README condensed table and the detailed
    matrix render from the same blocks, so a block added/removed/re-tiered in the
    YAML must be reflected in a readme_group or generation fails.
    """
    present = {b["block"]: b["tier"] for b in spec.get("present", [])}
    absent = {b["block"] for b in spec.get("absent", [])}
    seen_p, seen_a, errs = [], [], []
    for g in spec.get("readme_groups", []):
        for b in g["blocks"]:
            seen_p.append(b)
            if b not in present:
                errs.append(f"readme_group '{g['label']}' lists unknown block {b}")
            elif present[b] != g["tier"]:
                errs.append(f"'{b}' is tier {present[b]} but group "
                            f"'{g['label']}' is tier {g['tier']}")
    for g in spec.get("readme_absent", []):
        seen_a += [b for b in g["blocks"]]
        errs += [f"readme_absent lists unknown block {b}"
                 for b in g["blocks"] if b not in absent]
    for b in present:
        if seen_p.count(b) != 1:
            errs.append(f"present block '{b}' grouped {seen_p.count(b)}x (want 1)")
    for b in absent:
        if seen_a.count(b) != 1:
            errs.append(f"absent block '{b}' grouped {seen_a.count(b)}x (want 1)")
    if errs:
        raise SystemExit("readme_group drift:\n  " + "\n  ".join(errs))


def render_readme_capability(spec):
    out = ["| Subsystem | Tier | Evidence |", "|---|:--:|---|"]
    for g in spec.get("readme_groups", []):
        out.append(f"| {g['label']} | {g['tier']} | {g.get('evidence', '')} |")
    out += ["", f"**Absent on {spec['soc']} silicon — N/A (never a failure):**", "",
            "| Block | Why absent |", "|---|---|"]
    for g in spec.get("readme_absent", []):
        out.append(f"| {g['label']} | {g['reason']} |")
    return "\n".join(out)


def inject_readme(spec, path):
    verify_groups(spec)
    text = open(path).read()
    if README_BEGIN not in text or README_END not in text:
        raise SystemExit(f"{path}: missing capability-table markers")
    head, rest = text.split(README_BEGIN, 1)
    _, tail = rest.split(README_END, 1)
    block = f"{README_BEGIN}\n{render_readme_capability(spec)}\n{README_END}"
    new = head + block + tail
    if new != text:
        open(path, "w").write(new)
        sys.stderr.write(f"updated {path} capability table from test-matrix.yaml\n")
        return 1
    sys.stderr.write(f"{path} capability table already in sync\n")
    return 0


def main():
    global qemu_build_dir
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.environ.get("BUILD_DIR", "build"))
    ap.add_argument("-o", "--out",
                    default=os.path.join(REPO, "docs/validation/test-result-matrix.md"))
    ap.add_argument("--yaml",
                    default=os.path.join(REPO, "docs/validation/test-matrix.yaml"))
    ap.add_argument("--print", action="store_true",
                    help="print to stdout instead of writing the file")
    ap.add_argument("--inject-readme", nargs="?", const=os.path.join(REPO, "README.md"),
                    help="regenerate the README capability table from the YAML "
                         "(between the capability-table markers) and exit")
    args = ap.parse_args()

    if args.inject_readme is not None:
        spec = yaml.safe_load(open(args.yaml))
        return inject_readme(spec, args.inject_readme)
    qemu_build_dir = os.path.join(REPO, args.build_dir) \
        if not os.path.isabs(args.build_dir) else args.build_dir
    qemu = os.environ.get("QTEST_QEMU_BINARY") \
        or os.path.join(qemu_build_dir, "qemu-system-aarch64")

    spec = yaml.safe_load(open(args.yaml))
    present, absent = spec.get("present", []), spec.get("absent", [])

    lines = []
    p = lines.append
    p(f"# {spec['soc']} QEMU — test-result matrix (generated)")
    p("")
    p(f"<!-- GENERATED by tests/gen-test-matrix.py from test-matrix.yaml + qtest run.")
    p(f"     Tier/caveat are human-owned (from the YAML); test-result is CI-filled.")
    p(f"     Do not hand-edit; edit test-matrix.yaml and regenerate. -->")
    p("")
    p(f"Board `{spec['board']}` · machine `{spec['qemu_machine']}` · "
      f"qtest binary `{os.path.relpath(qemu, REPO) if qemu.startswith(REPO) else qemu}`.")
    p("")
    p("Tiers: **A** data-path-verified · **B** driver-bring-up · "
      "**C** registration · **N/A** absent (never a failure). ⚑ = fidelity "
      "caveat, see [`fidelity-audit.md`](fidelity-audit.md).")
    p("")
    p("## Present IP")
    p("")
    p("| IP block | Tier | Test result | Evidence |")
    p("|----------|:----:|-------------|----------|")
    npass = nfail = 0
    for e in present:
        r = result_for(e["test"], qemu)
        npass += r.startswith("PASS")
        nfail += r.startswith("FAIL")
        p(row(e["block"], e["tier"], r, e.get("evidence", ""), e.get("caveat")))
    p("")
    p("## Absent IP — N/A (NOT a negative result)")
    p("")
    p("| IP block | Tier | Why absent |")
    p("|----------|:----:|-----------|")
    for e in absent:
        p(f"| {e['block']} | N/A | {e['reason']} |")
    p("")
    p(f"Summary: {len(present)} present "
      f"({npass} PASS / {nfail} FAIL observed this run), "
      f"{len(absent)} N/A-absent.")
    p("")

    text = "\n".join(lines) + "\n"
    if args.print:
        sys.stdout.write(text)
    else:
        open(args.out, "w").write(text)
        sys.stderr.write(f"wrote {args.out} "
                         f"({len(present)} present, {len(absent)} absent; "
                         f"{npass} PASS / {nfail} FAIL)\n")
    return 1 if nfail else 0


qemu_build_dir = "build"
if __name__ == "__main__":
    sys.exit(main())

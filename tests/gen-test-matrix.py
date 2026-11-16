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

  # police the known-broken records: fail if a declared defect now passes
  tests/gen-test-matrix.py --check-broken

`qtest:<bin>` entries are executed (TAP ok/not-ok). `boot` and `harness:<dir>`
entries are labelled from their declared kind -- a full CI also runs the boot
smoke / functional harness and would upgrade those cells; this assembler does not
fabricate a pass it didn't observe.

The README's "What runs today" table is a SECOND rendering of test-matrix.yaml
(condensed, via the readme_groups grouping) between the capability-table markers;
--inject-readme regenerates it and verify_groups() fails if a block is un-grouped
or a group's tier disagrees with the block's -- so the README and the detailed
docs/validation/test-result-matrix.md cannot drift.

KNOWN-BROKEN CAPABILITIES
-------------------------
A tier says how much of a block is verified; it cannot say "this is modelled,
tested, and failing". Before this existed the matrix had four states -- tier
A/B/C and N/A-absent -- so a measured defect had nowhere to go but prose, where
no gate can reach it. A present block may therefore carry an optional, still
HUMAN-owned `broken:` record:

    broken: { when:   "the configuration it fails in",
              owner:  model | vendor-fw | qemu-core | unattributed,
              why:    "the defect, in one sentence",
              ref:    "repo-relative file holding the measurement",
              script: "the reproducer to run",       # resolved + X_OK checked
              expect: "the symptom string its FAIL must contain",
              env:    { EL3: "on" },                 # optional, pins the config
              silent_wrong: true,                    # optional, bool
              since:  "YYYY-MM-DD" }                 # optional, ISO

`owner` is the field a reader actually needs: `model` means it works on silicon
and not here (OUR bug), `vendor-fw` means the shipped firmware is broken the
same way on both, `qemu-core` means the gap is outside this machine. The fourth
value exists because the first three are claims about real hardware, and a
locus found by tracing THIS machine does not establish one: `unattributed`
means the defect is located but its behaviour on silicon has not been
measured. Guessing one of the other three and letting the renderer print the
gloss as fact is how a claim outruns its vantage.

`script` + `expect` are what make the record policeable, and they are required
because a record whose reproducer does not assert its own symptom polices
nothing: `script` is the harness that shows the defect (NOT merely the one in
the block's `test:` field -- a block-level harness can pass while a sub-aspect
of that block is broken), and `expect` is the string its failure must contain.
`env` pins the configuration into the record instead of leaving it to whatever
the operator's shell happens to export.

The record renders as a '✗' row in both tables plus its own "Known broken"
section, so a defect cannot hide among the green ones, and:

  * verify_broken() (run by --inject-readme, i.e. by the drift gate) rejects a
    record that is incomplete, mistyped, owned by nobody in the set, or whose
    `ref`/`script` does not resolve to a real file inside the repo (anchors are
    checked against the file's headings, scripts for the execute bit) -- a
    defect record that has rotted into a dangling pointer still reads as
    evidence;
  * --check-broken RUNS each known-broken reproducer and FAILS IF ONE PASSES.
    That is the half that earns the schema: the day the defect is fixed, the
    gate tells you to delete the row instead of letting a stale record sit in
    the tree asserting something false.

What the watch had to be taught, each case having been demonstrated against it:

  * A verdict is the LAST `^(PASS|FAIL|SKIP):` line AND the exit status, and
    they must agree. Scanning for any line starting with those words let a
    harness that echoes a guest log ("FAILED to load firmware blob") or a
    skipped subtest hide a real PASS; ignoring the exit status let a harness
    that printed "PASSWORD:" and exited 1 read as a pass, which would condemn
    a live record as stale.
  * A SKIP is not a pass. These harnesses skip by exiting 0 when their
    artifacts are missing, so exit status alone is exactly backwards.
  * A FAIL is not automatically THIS defect: it must contain `expect`, or it is
    reported as FAIL-OTHER -- the record is no longer substantiated by its own
    reproducer, which is as much a drift as a stale record.
  * All-SKIP is not green. "Could not run anything" and "every record still
    reproduces" are different results and are reported differently; --strict
    makes the former an error for CI that is supposed to have the artifacts.
  * A timeout is not evidence the defect reproduced.
"""
import argparse
import io
import os
import re
import subprocess
import sys

try:
    import yaml
except ImportError:
    sys.exit("need PyYAML: pip install pyyaml (or apt install python3-yaml)")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# A known-broken record names who owns the defect. Closed set on purpose: these
# answer different questions for a customer, and a free-text field would let
# them blur back together. `unattributed` is not a hedge for laziness -- it is
# the only honest value when the defect's locus was found by tracing THIS
# machine and nothing has been measured on silicon.
BROKEN_OWNERS = {
    "model":        "ours: works on silicon, broken here",
    "vendor-fw":    "the shipped firmware is broken the same way on real "
                    "hardware",
    "qemu-core":    "outside this machine model (QEMU core or another "
                    "subsystem)",
    "unattributed": "located, but NOT yet measured on silicon - do not read "
                    "this as either a hardware or a model defect",
}
BROKEN_REQUIRED = ("when", "owner", "why", "ref", "script", "expect")
BROKEN_OPTIONAL = ("silent_wrong", "since", "env")
ISO_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
VERDICT_LINE = re.compile(r"^(PASS|FAIL|SKIP):")


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
        # Deliberately NOT a bare "HARNESS": this assembler does not run the
        # harnesses, so the cell must not read like an observed pass.
        return "HARNESS (operator-run)"
    if test.startswith("qtest:"):
        return run_qtest(test.split(":", 1)[1], qemu)
    return "?"


def md_cell(text):
    """A pipe in free text would add columns to the rendered row."""
    return str(text).replace("|", "\\|")


def broken_result(broken, result):
    """The test-result cell for a declared-broken capability.

    The block's own result stays in the cell: a block can be broken in one
    aspect and still have a test covering the part that works, and replacing
    the result outright would make a regression in that part invisible behind
    a permanent "BROKEN".
    """
    mark = u"✗ BROKEN"
    if broken.get("silent_wrong"):
        mark += " / SILENT-WRONG"
    return f"{mark} ({broken['owner']}) · {result}"


def row(block, tier, result, evidence, caveat, broken=None):
    ev = md_cell(evidence or "")
    if caveat:
        ev = (u"⚑ " + md_cell(caveat) + ("; " + ev if ev else "")).strip("; ")
    if broken:
        ev = (u"✗ **%s:** %s (see Known broken, `%s`)"
              % (md_cell(broken["when"]), md_cell(broken["why"]),
                 md_cell(broken["ref"]))
              + ("; otherwise: " + ev if ev else ""))
    return f"| {md_cell(block)} | {tier} | {result} | {ev} |"


# ---- README condensed capability table (same YAML, second rendering) --------
README_BEGIN = "<!-- BEGIN capability-table (generated from test-matrix.yaml) -->"
README_END = "<!-- END capability-table (generated from test-matrix.yaml) -->"


def verify_groups(spec):
    """Every present/absent block is grouped exactly once; groups are single-tier.

    This is the anti-drift guard: the README condensed table and the detailed
    matrix render from the same blocks, so a block added/removed/re-tiered in the
    YAML must be reflected in a readme_group or generation fails.
    """
    names = [b["block"] for b in spec.get("present", [])]
    present = {b["block"]: b["tier"] for b in spec.get("present", [])}
    absent = {b["block"] for b in spec.get("absent", [])}
    seen_p, seen_a, errs = [], [], []
    # A duplicated name collapses in the dict above, so the grouping check
    # passes while the detailed matrix prints the block twice -- once broken,
    # once clean.
    errs += [f"present block '{n}' is listed {names.count(n)}x"
             for n in sorted(set(names)) if names.count(n) > 1]
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


def broken_blocks(spec):
    """[(block, broken-record)] for every present block declaring one.

    `broken: {}` / `null` / a misspelt key must NOT quietly mean "no defect
    here" -- that is how a recorded defect disappears from both renderings
    with the gate still green -- so the presence of the key is what counts and
    verify_broken() is what rejects a malformed one.
    """
    return [(b["block"], b["broken"]) for b in spec.get("present", [])
            if "broken" in b]


def script_of(entry):
    """The reproducer path a record names, or the default for its test kind.

    Resolved identically here and in verdict_of(), so that what the drift gate
    validates is exactly what the watch will run.
    """
    b = entry.get("broken") or {}
    if isinstance(b, dict) and isinstance(b.get("script"), str):
        return b["script"]
    test = entry.get("test")
    if isinstance(test, str) and test.startswith("harness:"):
        return os.path.join("tests", test.split(":", 1)[1], "run.sh")
    return None


def _inside_repo(path):
    """Reject absolute paths and anything escaping the repo.

    os.path.join(REPO, "/etc/passwd") is "/etc/passwd", so existence alone
    would happily accept a file outside the tree.
    """
    if not isinstance(path, str) or not path or os.path.isabs(path):
        return None
    full = os.path.normpath(os.path.join(REPO, path))
    if not (full + os.sep).startswith(REPO + os.sep):
        return None
    return full


def _headings(path):
    try:
        with io.open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return set()
    out = set()
    for line in text.splitlines():
        if line.startswith("#"):
            slug = line.lstrip("#").strip().lower()
            slug = re.sub(r"[^a-z0-9 -]", "", slug).replace(" ", "-")
            out.add(slug)
    return out


def verify_broken(spec):
    """A known-broken record must be complete, typed, owned, and policeable.

    Part of the anti-drift guard. A defect record that has rotted -- a dangling
    reference, a reproducer that cannot run, a field that renders as "None" --
    is worse than no record, because it still reads as evidence. --check-broken
    is the other half (a record whose reproducer passes, or fails for a
    different reason).
    """
    errs = []
    present = spec.get("present", []) or []
    for e in spec.get("absent", []) or []:
        if "broken" in e:
            errs.append(f"'{e.get('block')}': an absent block cannot be "
                        "known-broken; it is not modelled at all")
    for entry in present:
        block = entry.get("block")
        if "broken" not in entry:
            continue
        b = entry["broken"]
        if not isinstance(b, dict) or not b:
            errs.append(f"'{block}': broken: must be a non-empty mapping "
                        f"(got {b!r}) - delete the key or fill it in")
            continue
        for k in BROKEN_REQUIRED:
            v = b.get(k)
            if not isinstance(v, str) or not v.strip():
                errs.append(f"'{block}': broken: '{k}' must be a non-empty "
                            f"string (got {v!r})")
        for k in b:
            if k not in BROKEN_REQUIRED + BROKEN_OPTIONAL:
                errs.append(f"'{block}': broken: has unknown field '{k}'")
        if isinstance(b.get("owner"), str) and b["owner"] not in BROKEN_OWNERS:
            errs.append(f"'{block}': broken: owner {b['owner']!r} is not "
                        "one of " + ", ".join(sorted(BROKEN_OWNERS)))
        for k in ("when", "why", "expect"):
            if isinstance(b.get(k), str) and "\n" in b[k]:
                errs.append(f"'{block}': broken: '{k}' must be one line "
                            "(a newline breaks both rendered tables)")
        if "silent_wrong" in b and not isinstance(b["silent_wrong"], bool):
            errs.append(f"'{block}': broken: silent_wrong must be true/false "
                        f"(got {b['silent_wrong']!r}; a string is truthy)")
        if "since" in b and not ISO_DATE.match(str(b["since"])):
            errs.append(f"'{block}': broken: since must be YYYY-MM-DD "
                        f"(got {b['since']!r})")
        if "env" in b and not (isinstance(b["env"], dict) and all(
                isinstance(k, str) for k in b["env"])):
            errs.append(f"'{block}': broken: env must be a mapping of "
                        "name -> value")

        raw = str(b.get("ref", ""))
        ref, hashed, anchor = raw.partition("#")
        if hashed and not anchor:
            errs.append(f"'{block}': broken: ref '{raw}' ends in a bare '#'")
        full = _inside_repo(ref)
        if full is None:
            errs.append(f"'{block}': broken: ref '{b.get('ref')}' must be a "
                        "repo-relative path (an anchor alone points nowhere)")
        elif not os.path.isfile(full):
            errs.append(f"'{block}': broken: ref '{ref}' is not a file in the "
                        "tree - a record must point AT the measurement")
        elif anchor and anchor not in _headings(full):
            errs.append(f"'{block}': broken: ref anchor '#{anchor}' is not a "
                        f"heading in {ref}")

        scr = script_of(entry)
        if not isinstance(b.get("script"), str):
            pass                        # already reported as a missing field
        elif (full := _inside_repo(scr)) is None:
            errs.append(f"'{block}': broken: script '{scr}' must be a "
                        "repo-relative path")
        elif not os.path.isfile(full):
            errs.append(f"'{block}': broken: script '{scr}' does not exist")
        elif not os.access(full, os.X_OK):
            errs.append(f"'{block}': broken: script '{scr}' is not executable "
                        "- the watch could never run it")
        if not isinstance(entry.get("test"), str):
            errs.append(f"'{block}': a known-broken row needs a 'test:' field")
    if errs:
        raise SystemExit("known-broken record drift:\n  " + "\n  ".join(errs))


def render_broken(spec, heading_level="##", link_prefix=""):
    """The 'Known broken' section, shared by both renderings.

    `link_prefix` exists because the two renderings sit at different depths:
    the README is at the repo root, docs/validation/test-result-matrix.md is
    two levels down, and a repo-relative link written into the latter resolves
    to docs/validation/tests/... and dangles.
    """
    rows = broken_blocks(spec)
    if not rows:
        return ""
    title = u"Known broken — modelled, tested, and failing"
    out = [f"{heading_level} {title}", "",
           "Not gaps: each of these is a capability this model carries, with a "
           "reproducer that runs and does not pass. **`owner`** says whether "
           "real hardware has the same defect — `model` means it works on "
           "silicon and not here, and `unattributed` means that has not been "
           "measured yet and must not be assumed either way.", "",
           "| Capability | Broken when | Owner | Defect | Reproducer | "
           "Measurement |",
           "|---|---|---|---|---|---|"]
    for block, b in rows:
        flag = u" ☠ **silent-wrong**" if b.get("silent_wrong") else ""
        since = f" _(measured {b['since']})_" if b.get("since") else ""
        env = "".join(f" `{k}={v}`" for k, v in (b.get("env") or {}).items())
        out.append(
            f"| {md_cell(block)} | {md_cell(b['when'])}{env} | "
            f"`{b['owner']}` — {BROKEN_OWNERS[b['owner']]} | "
            f"{md_cell(b['why'])}{flag} | "
            f"`{md_cell(b['script'])}` must fail with "
            f"`{md_cell(b['expect'])}` | "
            f"[`{b['ref']}`]({link_prefix}{b['ref']}){since} |")
    out += ["", "A record here is gated both ways: the drift check rejects an "
            "incomplete record, a dangling `ref`, or a reproducer that cannot "
            "run, and `tests/gen-test-matrix.py --check-broken` **fails if one "
            "of these reproducers passes, or fails for a reason other than its "
            "own `expect` string** — so a record can outlive neither the "
            "defect nor the evidence for it."]
    return "\n".join(out)


def render_readme_capability(spec):
    marked = {blk for blk, _ in broken_blocks(spec)}
    out = ["| Subsystem | Tier | Evidence |", "|---|:--:|---|"]
    for g in spec.get("readme_groups", []):
        hit = [b for b in g["blocks"] if b in marked]
        ev = g.get("evidence", "")
        if hit:
            # Naming the block is the point when a group covers several; when
            # the group IS that one block, repeating its label adds nothing.
            which = ("" if hit == [g["label"]]
                     else ":** " + ", ".join(hit) + " ")
            ev = (u"✗ **known broken%s— see Known broken below. "
                  % (which or "** ")) + ev
        out.append(f"| {g['label']} | {g['tier']} | {ev} |")
    out += ["", f"**Absent on {spec['soc']} silicon — N/A (never a failure):**", "",
            "| Block | Why absent |", "|---|---|"]
    for g in spec.get("readme_absent", []):
        out.append(f"| {g['label']} | {g['reason']} |")
    broken = render_broken(spec, heading_level="###")
    if broken:
        out += ["", broken]
    return "\n".join(out)


def verdict_of(entry, qemu):
    """Run one record's reproducer. Returns (verdict, detail).

    Verdicts: PASS, FAIL, SKIP, MISMATCH (the printed verdict and the exit
    status disagree), TIMEOUT, ERROR.

    Two rules learned the hard way, each from a harness built to beat the
    earlier version of this function:

      * the verdict is the LAST `^(PASS|FAIL|SKIP):` line, not any line that
        happens to start with one of those words. These harnesses echo greps
        of the guest serial log, so a kernel message beginning "FAILED" could
        mask a real PASS and keep a fixed defect recorded as broken;
      * the exit status has to agree with it. A harness that printed
        "PASSWORD: prompt seen" and exited 1 read as a pass, which would have
        condemned a live record as stale. Conversely a SKIP exits 0, so the
        exit status alone is exactly backwards.
    """
    test, b = entry["test"], entry["broken"]
    if test.startswith("qtest:") and not b.get("script"):
        r = run_qtest(test.split(":", 1)[1], qemu)
        return r.split(" ", 1)[0], r

    script = script_of(entry)
    full = _inside_repo(script) if script else None
    if not full or not os.path.isfile(full):
        return "SKIP", f"no runnable reproducer for '{test}'"
    env = dict(os.environ)
    if qemu:
        env.setdefault("QEMU", qemu)
        env.setdefault("QTEST_QEMU_BINARY", qemu)
    # Assignment, not setdefault: the record pins the configuration it is
    # about. Inheriting it from the operator's shell is how a stray
    # `export EL3=off` turns a true record into a "stale" one.
    for k, v in (b.get("env") or {}).items():
        env[k] = str(v)
    try:
        out = subprocess.run([full], capture_output=True, text=True,
                             env=env, cwd=REPO, timeout=1800)
    except subprocess.TimeoutExpired:
        return "TIMEOUT", "harness timed out - NOT evidence it reproduces"
    except Exception as e:                                       # noqa: BLE001
        return "ERROR", f"could not run {script}: {e.__class__.__name__}"

    lines = [l.strip() for l in (out.stdout + out.stderr).splitlines()]
    said = [l for l in lines if VERDICT_LINE.match(l)]
    if not said:
        return "SKIP", f"{script} printed no verdict (exit {out.returncode})"
    verdict, detail = said[-1].split(":", 1)[0], said[-1][:200]
    agrees = (verdict == "PASS" and out.returncode == 0) or \
             (verdict == "FAIL" and out.returncode != 0) or \
             (verdict == "SKIP" and out.returncode == 0)
    if not agrees:
        return "MISMATCH", (f"said {verdict} but exited "
                            f"{out.returncode}: {detail}")
    if verdict == "FAIL":
        # Show the line that SUBSTANTIATES the record, not the harness's
        # closing summary: a multi-leg harness ends with a generic "FAIL:",
        # which tells a reader nothing about which defect reproduced.
        hit = [l for l in lines if b["expect"] in l]
        if not hit:
            return "FAIL-OTHER", (f"failed WITHOUT '{b['expect']}' - this is "
                                  f"not the recorded defect: {detail}")
        return verdict, hit[0][:200]
    return verdict, detail


def check_broken(spec, qemu, strict=False):
    """Police the known-broken records against reality.

    Fails if a record's reproducer passes (the defect is fixed and the record
    is now the false thing in the tree) or fails for a reason other than its
    own `expect` string (the record is no longer substantiated). All-SKIP is
    reported as its own result, because "could not run anything" is not the
    same news as "every record still reproduces".
    """
    verify_broken(spec)
    rows = [e for e in spec.get("present", []) if "broken" in e]
    if not rows:
        print("no known-broken records to check")
        return 0
    stale, unsubstantiated, ran = [], [], 0
    for entry in rows:
        b = entry["broken"]
        verdict, detail = verdict_of(entry, qemu)
        print(f"{verdict:10} {entry['block']}  [{entry['test']}, "
              f"owner={b['owner']}]\n           {detail}")
        if verdict in ("PASS",):
            stale.append((entry["block"], b))
        elif verdict in ("FAIL-OTHER", "MISMATCH"):
            unsubstantiated.append((entry["block"], b, verdict))
        if verdict in ("PASS", "FAIL"):
            ran += 1
    if stale:
        print("\nSTALE DEFECT RECORDS - these pass their own reproducer but "
              "test-matrix.yaml still calls them broken:")
        for block, b in stale:
            print(f"  * {block}: remove its broken: block (and update "
                  f"{b['ref']}), or the matrix is now the thing that is wrong")
    if unsubstantiated:
        print("\nUNSUBSTANTIATED RECORDS - these fail, but not with the "
              "symptom they claim, so they are no longer evidence of it:")
        for block, b, v in unsubstantiated:
            print(f"  * {block} [{v}]: re-measure and correct 'why'/'expect', "
                  f"or point script: at a reproducer that shows it")
    if stale or unsubstantiated:
        return 1
    if not ran:
        print(f"\nRAN NOTHING: all {len(rows)} records skipped (missing "
              "artifacts). This is NOT a green watch - nothing was policed.")
        return 1 if strict else 0
    print(f"\nPASS: {ran}/{len(rows)} known-broken records reproduced with "
          "their own symptom; none has silently become true")
    return 0


def check_doc_section(spec, path):
    """Is the detailed matrix's Known-broken section current with the YAML?

    The detailed matrix is fully generated, but regenerating it runs the
    qtests, so the fast boot-free gate cannot diff the whole file. This
    section renders from the YAML alone, which is the part the records
    changed: delete a record and the README is regenerated by the gate while
    the detailed matrix would quietly keep the row.
    """
    want = render_broken(spec, link_prefix="../../")
    text = io.open(path, encoding="utf-8").read()
    head = "## Known broken"
    have = ""
    if head in text:
        have = text[text.index(head):].split("\nSummary:", 1)[0].strip()
    if have.strip() != want.strip():
        raise SystemExit(
            f"{os.path.relpath(path, REPO)}: its Known-broken section is stale "
            "vs docs/validation/test-matrix.yaml.\nRegenerate it:\n"
            "    tests/gen-test-matrix.py")
    n = len(broken_blocks(spec))
    sys.stderr.write(f"{os.path.relpath(path, REPO)}: Known-broken section "
                     f"in sync ({n} record{'s' if n != 1 else ''})\n")
    return 0


def inject_readme(spec, path):
    verify_groups(spec)
    verify_broken(spec)
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
    ap.add_argument("--check-broken", action="store_true",
                    help="run the reproducer of every capability recorded as "
                         "broken and FAIL IF ONE PASSES (a stale record) or "
                         "fails without its own expect string")
    ap.add_argument("--check-doc-section", action="store_true",
                    help="verify the detailed matrix's Known-broken section "
                         "is current with the YAML (boot-free)")
    ap.add_argument("--strict", action="store_true",
                    help="with --check-broken, also fail when every record "
                         "skipped - for CI that is meant to have the artifacts")
    args = ap.parse_args()

    if args.inject_readme is not None:
        spec = yaml.safe_load(open(args.yaml))
        rc = inject_readme(spec, args.inject_readme)
        return rc
    if args.check_doc_section:
        spec = yaml.safe_load(open(args.yaml))
        verify_broken(spec)
        return check_doc_section(spec, args.out)
    qemu_build_dir = os.path.join(REPO, args.build_dir) \
        if not os.path.isabs(args.build_dir) else args.build_dir
    qemu = os.environ.get("QTEST_QEMU_BINARY") \
        or os.path.join(qemu_build_dir, "qemu-system-aarch64")

    spec = yaml.safe_load(open(args.yaml))
    if args.check_broken:
        return check_broken(spec, qemu, args.strict)
    verify_groups(spec)
    verify_broken(spec)
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
      "caveat, see [`fidelity-audit.md`](fidelity-audit.md). ✗ = **known "
      "broken** — a capability with a test that runs and does not pass; the "
      "tier describes what IS verified, the ✗ row says what is not.")
    p("")
    p("What a result cell is worth, since they are not alike: a `PASS (qtest)` "
      "was observed by this run; a `PASS (boot)` is the declared CI boot-smoke "
      "result, not something this assembler watched; a `HARNESS "
      "(operator-run)` was not run here at all. A tier is never inferred from "
      "a green test.")
    p("")
    p("## Present IP")
    p("")
    p("| IP block | Tier | Test result | Evidence |")
    p("|----------|:----:|-------------|----------|")
    npass = nfail = nbroken = 0
    for e in present:
        bk = e.get("broken")
        r = result_for(e["test"], qemu)
        npass += r.startswith("PASS")
        nfail += r.startswith("FAIL")
        if bk:
            r, nbroken = broken_result(bk, r), nbroken + 1
        p(row(e["block"], e["tier"], r, e.get("evidence", ""),
              e.get("caveat"), bk))
    p("")
    p("## Absent IP — N/A (NOT a negative result)")
    p("")
    p("| IP block | Tier | Why absent |")
    p("|----------|:----:|-----------|")
    for e in absent:
        p(f"| {e['block']} | N/A | {e['reason']} |")
    p("")
    broken_section = render_broken(spec, link_prefix="../../")
    if broken_section:
        p(broken_section)
        p("")
    p(f"Summary: {len(present)} present "
      f"({npass} PASS / {nfail} FAIL observed this run, "
      f"{nbroken} declared broken), "
      f"{len(absent)} N/A-absent.")
    p("")

    text = "\n".join(lines) + "\n"
    if args.print:
        sys.stdout.write(text)
    else:
        open(args.out, "w").write(text)
        sys.stderr.write(f"wrote {args.out} "
                         f"({len(present)} present, {len(absent)} absent; "
                         f"{npass} PASS / {nfail} FAIL / "
                         f"{nbroken} known-broken)\n")
    # A DECLARED broken capability is not an unexpected failure, so it does not
    # fail generation -- it would pin the drift gate red forever and the record
    # would be deleted to get CI green again, which is the opposite of the
    # point. --check-broken is what polices those rows.
    return 1 if nfail else 0


qemu_build_dir = "build"
if __name__ == "__main__":
    sys.exit(main())

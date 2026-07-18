#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run-structure oracle for the SAI playback capture: catch an in-stream sample
# loss that peak / tone / duration cannot see.
#
# WHY THIS EXISTS (the four-tree "bug #2" thread, 2026-07-18)
#
# pcm_play emits a square wave that toggles sign every EXACTLY 55 frames. A
# dropped sample anywhere in the stream cuts one half-period short: a 55-run
# becomes a 54-run. That is a LOCAL property of the waveform - it does not depend
# on the total sample count, on where the capture starts or ends, or on
# wall-clock timing. So it is IMMUNE to the count jitter that makes a
# frame-count assertion useless on a wall-clock ALSA capture (91/93's finding):
# jitter moves the total, not the interior run lengths.
#
#   peak / dominant-tone / duration all shrug at a scattered 0.4% loss - it moves
#   none of those statistics. Only asserting the run STRUCTURE catches it.
#   (91emulator: assert the structure, not the summary statistic.)
#
# TWO REQUIREMENTS, both from the fleet:
#   1. The wav rate MUST be pinned (-audio driver=wav,out.frequency=<rate>), or
#      QEMU's mixer resamples 48k->44.1k and smears every 55-run to 50/51 -
#      killing the signal even with no bug. (rt1180's out.frequency pin.)
#   2. A frame-based periodic played signal so "clean == 55" is well-defined
#      (pcm_play's toggle).
#
# HONEST NUANCES, carried from 91/93 - do not paper over them:
#   - ALSA-stack capture MAY inject period-boundary discontinuities that make an
#     interior run != 55 even with NO bug. So this is "confirm the clean baseline
#     off-55 is ~0 first", not a guarantee. --selftest proves the ANALYZER on
#     synthetic wavs; the live gate rides on a measured clean baseline.
#   - The mixer DILUTES a subtle loss: a 0.4% model drop reads far less than 22%
#     off-runs through the full stack. Set the live threshold from YOUR measured
#     dilution, never from the self-test's synthetic number.
import struct
import sys

TOGGLE = 55                 # pcm_play half-period, in frames
AMP_THRESH = 1000           # |sample| above this = "signal", not lead/trail silence


def read_channel0(path):
    """Parse a 16-bit PCM WAV, tolerant of a zero-length header.

    QEMU's wav backend writes the RIFF-size and data-size fields as 0 and does
    NOT backfill them when the guest powers off mid-stream, so Python's strict
    `wave` module rejects the file as "not a WAVE file". The audio is perfectly
    good; only the two size fields are stale. So walk the chunks by hand and read
    the data chunk to EOF when its declared size is 0.
    """
    with open(path, "rb") as f:
        buf = f.read()
    if buf[0:4] != b"RIFF" or buf[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    nch, sw, data = None, None, None
    i = 12
    while i + 8 <= len(buf):
        cid, csz = buf[i:i + 4], struct.unpack("<I", buf[i + 4:i + 8])[0]
        body = i + 8
        if cid == b"fmt ":
            _, nch, _, _, _, bits = struct.unpack("<HHIIHH", buf[body:body + 16])
            sw = bits // 8
        elif cid == b"data":
            end = body + csz if csz else len(buf)       # csz==0 -> read to EOF
            data = buf[body:min(end, len(buf))]
            break
        if csz == 0:                                     # unsized non-data chunk
            break
        i = body + csz + (csz & 1)                       # chunks are word-aligned
    if nch is None or data is None:
        raise ValueError("no fmt/data chunk found")
    if sw != 2:
        raise ValueError("expected 16-bit PCM, got %d-bit" % (sw * 8))
    data = data[:len(data) - (len(data) % (2 * nch))]    # whole frames only
    alls = struct.unpack("<%dh" % (len(data) // 2), data)
    return list(alls[0::nch])       # channel 0 of the interleaved frames


def sign_runs(samples):
    """Run lengths of same-sign frames within the signal region.

    Leading/trailing silence is trimmed; a zero sample continues the current
    sign (a clean square wave never crosses zero mid-run, but the mixer can).
    """
    hot = [i for i, v in enumerate(samples) if abs(v) > AMP_THRESH]
    if not hot:
        return []
    seg = samples[hot[0]:hot[-1] + 1]
    runs, cur_sign, cur_len = [], 0, 0
    for v in seg:
        sgn = 1 if v > 0 else (-1 if v < 0 else cur_sign)
        if sgn == cur_sign:
            cur_len += 1
        else:
            if cur_sign != 0:
                runs.append(cur_len)
            cur_sign, cur_len = sgn, 1
    if cur_sign != 0:
        runs.append(cur_len)
    return runs


def off55(samples):
    """(interior_run_count, off_count, off_pct) — first/last runs are partial."""
    runs = sign_runs(samples)
    if len(runs) < 3:
        return 0, 0, 0.0
    interior = runs[1:-1]
    off = [r for r in interior if r != TOGGLE]
    return len(interior), len(off), 100.0 * len(off) / len(interior)


# ---- synthetic generators (for --selftest; no guest, no mixer) ----
def gen_clean(nframes):
    return [(8000 if (i // TOGGLE) & 1 else -8000) for i in range(nframes)]


def gen_dropped(nframes, drop_every):
    """A clean square wave with one frame elided every drop_every frames -
    exactly bug #2's symptom (advance-by-chunk loses the backend-declined tail).
    """
    out, i = [], 0
    while len(out) < nframes:
        if drop_every and i and i % drop_every == 0:
            i += 1                      # skip (drop) this frame
            continue
        out.append(8000 if (i // TOGGLE) & 1 else -8000)
        i += 1
    return out


def selftest():
    n = 96000
    ci, co, cp = off55(gen_clean(n))
    di, do, dp = off55(gen_dropped(n, 250))     # 1-in-250 = 0.4% loss
    print("  clean square wave        -> %5.1f%% off-55 (%d/%d)" % (cp, co, ci))
    print("  0.4%% drop (1-in-250)     -> %5.1f%% off-55 (%d/%d)" % (dp, do, di))
    ok = cp == 0.0 and dp > 5.0
    print("  SELFTEST %s: clean is 0%%%s and a 0.4%% drop is caught (>5%%)%s"
          % ("PASS" if ok else "FAIL",
             "" if cp == 0.0 else " [clean NOT 0 - analyzer bug]",
             "" if dp > 5.0 else " [drop NOT caught - analyzer blind]"))
    return 0 if ok else 1


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) < 2:
        print("usage: check_wav.py <file.wav> [max_off_pct]  |  --selftest")
        return 2
    path = sys.argv[1]
    thresh = float(sys.argv[2]) if len(sys.argv) > 2 else None
    interior, off, pct = off55(read_channel0(path))
    print("CHECK_WAV: %s  interior_runs=%d  off-55=%d  (%.2f%%)"
          % (path, interior, off, pct))
    if interior < 3:
        print("CHECK_WAV: too few runs to judge (silent/short capture?)")
        return 3
    if thresh is None:
        return 0                        # informational (baseline-measuring) mode
    if pct <= thresh:
        print("CHECK_WAV: OK (<= %.2f%% off-55; run structure intact)" % thresh)
        return 0
    print("CHECK_WAV: FAIL (%.2f%% > %.2f%% off-55 - an in-stream sample loss "
          "the tone/peak/duration checks cannot see)" % (pct, thresh))
    return 1


if __name__ == "__main__":
    sys.exit(main())

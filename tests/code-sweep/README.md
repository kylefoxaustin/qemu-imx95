# code-sweep — run real third-party software on the i.MX 95

A scoreboard harness that takes real upstream source, builds it, runs it **on
the emulated i.MX 95 machine**, and checks each program against its own
known-good oracle. The goal is breadth: saturate the A55 + peripheral surface
with as much real, correctness-checked code as possible.

Scope is deliberately **CPU + standard-peripheral only** — no GPU/VPU/NPU/ISP
workloads, since those blocks have no functional model.

## How it works

For every recipe in `recipes/`, `run.sh`:

1. fetches the upstream source tarball (cached under `build/code-sweep/cache`),
2. **cross-compiles** it statically on the host (`aarch64-linux-gnu-gcc`) into a
   per-item staging dir, next to a guest-side `runtest.sh` carrying the
   program's built-in oracle (self-test / round-trip / known-answer test),
3. exposes the staging tree to the guest over **virtio-9p**,
4. boots the busybox initramfs, mounts the share, runs every `runtest.sh`
   in-guest, and writes pass/fail + output back over 9p,
5. prints a scoreboard.

This is the **cross-compile-on-host → run-in-guest** tier: it proves the A55
executes real software correctly. An **in-guest build** tier (compile *and* run
on the machine) is future work — it needs a toolchain rootfs; the recipe format
is designed to host it too.

## Usage

```sh
tests/code-sweep/run.sh                 # all recipes
tests/code-sweep/run.sh zlib bzip2      # a subset by NAME
```

Required (override via env): `QEMU`, `KBUILD` (Image + dtb), `SM_ELF`, a busybox
initramfs (`INITRD`), and an aarch64 cross gcc (`CROSS`/`CC`). SKIPs (rc=0) if
any is missing — same convention as the other `tests/` harnesses.

## Adding a corpus item

Drop a `recipes/<name>.recipe` file. It is sourced in a subshell and must set:

- `NAME`, `CATEGORY`, `SRC` (tarball URL), `REF` (tag/version, used for caching)
- a `cs_build` function, run with cwd = extracted source and these exported:
  - `CC` / `AR` / `STRIP` — the cross toolchain
  - `OUT` — the per-item staging dir

`cs_build` must place runnable artifacts into `$OUT` and write an executable
`$OUT/runtest.sh` that runs **entirely in the guest**. Carry the oracle inside
`runtest.sh` (the upstream self-test, a round-trip hash compare, a KAT verdict,
etc.) so a wrong byte anywhere fails.

**Exit-code convention** (per the runtest.sh):
- `0` — PASS
- `77` — SKIP (autotools convention): a first-class outcome for a capability the
  *model* legitimately lacks (RNG-hw, crypto-accel, perf counters, GPU/VPU/NPU).
  A SKIP does **not** fail the run — use it instead of letting such a case FAIL,
  so the pass rate stays honest. Prefer naming the reason in the log.
- anything else — FAIL.

Each case is wrapped in `timeout` in the guest (`CASE_TMO`, default 300s) so a
single hung case under TCG can't eat the whole run; a timed-out case is a FAIL.
A host-side build failure is reported separately as `BLDFAIL`.

Pick programs whose correctness oracle ships with the source — that's the
highest coverage-per-effort, because no expected output has to be hand-curated.

## Current corpus

| item | category | oracle |
|------|----------|--------|
| `zlib` | compression | bundled `example` self-test + minigzip pipe round-trip (sha256) |
| `bzip2` | compression | upstream `make test` round-trip `cmp` against committed refs |
| `zstd` | compression | high-level compress + `zstd -t` integrity check + decompress (sha256) |
| `crypto-algorithms` | crypto | B-Con per-algorithm known-answer tests (sha256/sha1/md5/md2/aes/des/blowfish/arcfour/base64) |
| `xxhash` | hashing | canonical empty-input vectors (XXH32/64/128/XXH3) + host/guest agreement on a random payload |
| `sqlite` | database | SQL workload (DDL/index/join/aggregate/CTE/window/JSON1) diffed vs a host-built golden |
| `lua` | interpreter | reference interpreter runs a self-test asserting strings/tables/int-float/closures/coroutines/metatables |

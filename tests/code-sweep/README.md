# code-sweep — run real third-party software on the i.MX 95

A scoreboard harness that takes real upstream source, builds it, runs it **on
the emulated i.MX 95 machine**, and checks each program against its own
known-good oracle. The goal is breadth: saturate the A55 + peripheral surface
with as much real, correctness-checked code as possible.

Scope is deliberately **CPU + standard-peripheral only** — no GPU/VPU/NPU/ISP
workloads, since those blocks have no functional model.

**Results of record** (every item, version, oracle, status) are committed at
[`docs/validation/code-sweep-matrix.yaml`](../../docs/validation/code-sweep-matrix.yaml)
(machine-readable) and in the "Code-sweep" section of
[`docs/imx95/validation-report.md`](../../docs/imx95/validation-report.md).

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

## Peripheral tier (`run-peripheral.sh`)

A second harness that runs real code against the machine's actual **peripheral
datapaths**, not just the CPU — per the MCXN947 sweep's lesson that real drivers
+ real apps surface model bugs synthetic word-aligned tests never hit. The
machine boots with extra devices attached and the guest wires them up before
running `recipes-peripheral/`:

- an **eMMC** card (ext4, host-pre-formatted) on the uSDHC controller, mounted at
  `/data` — exercises the SD/eMMC write+read ADMA path
- **user networking** on the ENETC NIC (slirp; host at 10.0.2.2)

```sh
tests/code-sweep/run-peripheral.sh            # all peripheral recipes
```

Each peripheral recipe declares a `CS_KEY` from a **shared cross-machine
namespace** (agreed with the i.MX91 sweep: `storage-emmc`, `storage-sd`,
`usb-enum`, `usb-bulk`, `net-*`, `i2c-*`, `audio-*`, `rtc`, `watchdog`, `gpio`;
95-specific keys are `m95-*`). The guest emits `SOAK:PASS|FAIL|SKIP:<key>:…`
markers and the host renders a `function/PASS/FAIL/SKIP` dashboard over those
keys — so the 91 and 95 boards are literally diff-able and a future merge of
both STATE dirs prints one cross-machine board.

| item | datapath | oracle |
|------|----------|--------|
| `storage-sqlite` | uSDHC / eMMC | sqlite writes a 20k-row DB through ext4-on-eMMC, drops caches, re-reads off the card; rows diffed vs a host golden |
| `net-tftp` | ENETC NIC | static busybox brings eth0 up, pulls a 256 KB payload + sha over TFTP from slirp, sha256-verifies (boot patches the dtb so ENETC probes) |

## Current corpus (CPU tier)

| item | category | oracle |
|------|----------|--------|
| `zlib` | compression | bundled `example` self-test + minigzip pipe round-trip (sha256) |
| `bzip2` | compression | upstream `make test` round-trip `cmp` against committed refs |
| `zstd` | compression | high-level compress + `zstd -t` integrity check + decompress (sha256) |
| `xz` | compression | compress + `xz -t` integrity check + decompress round-trip (sha256) |
| `brotli` | compression | compress + decompress round-trip (sha256), CLI cross-built from source glob |
| `lz4` | compression | compress -9 + decompress round-trip (sha256) |
| `heatshrink` | compression | heatshrink's own encoder/decoder round-trip suite (greatest framework) |
| `miniz` | compression | miniz example1 compress/decompress round-trip with memcmp self-check |
| `pcre2` | regex | pcre2test runs PCRE2's own testinput1, diffed vs the committed testoutput1 (upstream RunTest test 1) |
| `oniguruma` | regex | Oniguruma's own testc/back/options/regset suites (SUCC/FAIL/ERROR), nonzero unless FAIL+ERROR==0 |
| `base64` | base64 | aklomp/base64's known-answer encode/decode vectors, nonzero on mismatch |
| `sds` | string | antirez sds self-test (-DSDS_TEST_MAIN): create/concat/cpy/catfmt/trim/split, requires 0 failed |
| `expat` | xml | xmlwf accepts a battery of well-formed XML and rejects malformed XML (both paths verified) |
| `tinyexpr` | math | bundled smoke test (parsing/precedence/functions/closures/pow), returns nonzero on any failed case |
| `utf8proc` | unicode | utf8proc's own self-contained tests (case-fold/charwidth/iterate/valid/misc), each exits nonzero on mismatch |
| `uthash` | datastruct | uthash's full test suite — 96 programs, each stdout compared to its committed `.ans` golden |
| `crypto-algorithms` | crypto | B-Con per-algorithm known-answer tests (sha256/sha1/md5/md2/aes/des/blowfish/arcfour/base64) |
| `libsodium` | crypto | ~20 of libsodium's own test/default programs, each self-checking vs its `.exp` (box/sign/secretbox/aead/scalarmult/…) |
| `mbedtls` | crypto | Mbed-TLS `selftest` KATs across ~22 suites (aes/gcm/sha/rsa/mpi/ecp/chacha20/…) |
| `libtomcrypt` | crypto | LibTomCrypt's own math-free KAT sub-tests (cipher/hash/MAC/modes/store/misc) |
| `xxhash` | hashing | canonical empty-input vectors (XXH32/64/128/XXH3) + host/guest agreement on a random payload |
| `jq` | json | jq's own `--run-tests` suite; differential oracle — A55 must reproduce the version-pinned host baseline (435/447, 9 malformed) |
| `cjson` | json | cJSON's own Unity test suites (parse/print/compare/minify/misc/add), each exits nonzero on assert failure |
| `sqlite` | database | SQL workload (DDL/index/join/aggregate/CTE/window/JSON1) diffed vs a host-built golden |
| `lua` | interpreter | reference interpreter runs a self-test asserting strings/tables/int-float/closures/coroutines/metatables |
| `cpython` | interpreter | static CPython 3.10 cross-built; runs its own `python -m test` stdlib regression suite (33 CPU-pure modules) in-guest |
| `ltp-syscalls` | syscall | LTP syscall subset cross-built static (323 binaries); each testcase is its own oracle — ~250 pass on the A55, TBROK/TCONF counted as SKIP |

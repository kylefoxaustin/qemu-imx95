# in-guest build tier — compile *and* run on the i.MX 95 A55

The strongest self-hosting signal for the board farm: a developer can build their
C code **on the emulated board** and run the result, not just cross-compile it on
a host. (The [`code-sweep`](../code-sweep/) CPU tier cross-compiles on the host
then runs in the guest; this compiles *and* runs entirely in the guest.)

Two variants, light and representative:

```sh
tests/in-guest-build/run.sh           # tcc + musl, busybox initramfs (lightweight)
tests/in-guest-build/run-gcc.sh       # real gcc/g++ + glibc rootfs off eMMC (representative)
tests/in-guest-build/run-gcc-build.sh # REAL upstream projects: ./configure/make + their own tests
tests/in-guest-build/run-clang.sh     # native LLVM/clang toolchain (the other major compiler)
```

Both compile every test **in-guest**, run each binary on the A55, and check its
stdout against the program's own `// EXPECT:` line (exit 0 iff every case builds
and prints the expected output; SKIP if a prerequisite is missing).

`run.sh` builds a tiny native toolchain **from source** (nothing is committed as a
binary), boots the busybox initramfs on `imx95-19x19-evk`, and compiles `tests/*.c`
with it. Prereqs: QEMU, kernel+dtb, SM ELF, a cross gcc.

`run-gcc.sh` is the representative variant — a **real native GCC** (glibc gcc/g++
+ libstdc++ + libm, GCC 14) on a Debian rootfs booted as the eMMC root filesystem,
compiling `gcc-tests/*.{c,cpp}` (incl. C++ STL). That's what a farm developer
actually does. Prereqs add **docker** (to fetch the arm64 rootfs) + `mke2fs`.

`run-gcc-build.sh` goes the extra step — it builds **real upstream projects** on
the A55 and runs each project's **own** test suite, proving the machine is a
working dev host (download → `./configure`/`make` → `make test` passes, all
in-guest). Corpus: **bzip2** (`make && make test`), **zlib** (`./configure &&
make && make test`), **lua** (`make posix`, then run a Lua script). Tarballs are
fetched host-side (cached) and staged into the rootfs, so the guest needs no
network. Real builds under TCG are slow — the default timeout is generous
(`TMO=1800`). Verified `pass=4 fail=0` (bzip2, zlib, lua, sqlite).

`run-clang.sh` is the same idea with the **other** major toolchain: a native
LLVM/clang (clang 19 + clang++ + lld) on an arm64 rootfs off eMMC, compiling
`clang-tests/*.{c,cpp}` in-guest (C + C++ STL). Links with `-fuse-ld=lld` (the
clang image ships lld, not GNU ld). Verified `pass=2 fail=0`. So the farm proves
both gcc and clang self-host on the board.

## Toolchain

- **TinyCC (native aarch64)** — a C compiler cross-built to *run on the A55* and
  emit arm64. tcc is itself real third-party code. (Its arm64 backend is young:
  its linker can't do glibc's GOT/TLS relocations and its inline asm lacks `svc`,
  which is why the libc is musl and the programs are ordinary libc C.)
- **musl libc (static)** — tcc's canonical libc companion; its static objects
  avoid the relocations glibc pulls. Linked `-static` because the initramfs has
  no dynamic loader.

Both are fetched + built by `run.sh` and cached under `build/in-guest-build/`
(gitignored). Hard-won build gotchas, all encoded in the script:

- the `c2str` build-helper must be compiled with the **host** cc (it runs during
  the tcc build), not the target cc;
- `libtcc1.a` (tcc's runtime) is normally produced by running tcc — since our tcc
  is an aarch64 binary, the arm64 runtime objects are compiled with the cross cc;
- **both** musl and libtcc1.a must be built `-fno-stack-protector` (Ubuntu gcc
  defaults to `-fstack-protector-strong`, and tcc links libtcc1 *after* libc, so
  the `__stack_chk_*` references otherwise go unresolved);
- tcc's crt prefix is the **triplet** libdir, so musl's `crt1.o/crti.o/crtn.o` +
  `libc.a` are staged in `/usr/lib/arm64-linux-gnu`, headers in `/usr/include`.

## The gcc rootfs (run-gcc.sh)

The aarch64 rootfs is the upstream multi-arch `gcc` Docker image:
`docker pull --platform linux/arm64` fetches a foreign-arch image **without
running it**, and `docker export` yields a complete native arm64 gcc — no cross or
emulation on the host. The script stages tests + a tiny init, builds an ext4 image
with `mke2fs -d` (no root), and boots it as `/dev/mmcblk0`. Gotchas (each cost a
boot):

- invoke gcc by **full path** (`/usr/local/bin/gcc`) — bare `gcc` computes a
  relative exec-prefix and can't find `cc1` ("cannot execute 'cc1'");
- `PATH` must include `/usr/bin` so `collect2` finds `ld` at link;
- the rootfs has no init system, so it boots `init=/igtest.sh` directly and the
  script **must never exit** (else "Attempted to kill init" panic) — it loops
  after poweroff;
- inject the script via `mke2fs -d` (rebuild the image), **not** `debugfs write`,
  which silently truncated it here.

## Adding a test

Drop a `tests/<name>.c` (tcc variant) or `gcc-tests/<name>.{c,cpp}` (gcc variant)
whose **first line** is `// EXPECT: <exact stdout>`. tcc/musl cases must stay
within tcc's arm64 codegen (loops, arithmetic, libc: `printf`, `qsort`, `malloc`,
`string.h`); gcc cases can use anything glibc/libstdc++ provide (C++ STL, libm,
…). Committed: tcc — `sum.c` (loops+printf), `fib.c` (arithmetic), `qsort.c` (musl
stdlib+fn ptrs); gcc — `sum.c`, `sqrt.c` (libm), `cppsort.cpp` (g++ + STL).

## Status / roadmap

All three tiers proven (`pass=3 fail=0` each): the A55 hosts a compiler, runs the
binaries it produces, and natively builds + tests real upstream projects.
`run.sh` (tcc+musl) is the lightweight capability proof; `run-gcc.sh` (real glibc
gcc/g++) is the representative toolchain; `run-gcc-build.sh` is the real-project
corpus. To grow the corpus, add a project to `run-gcc-build.sh` (a tarball URL +
a `./configure`/`make` + self-test block). Possible next steps: a larger app
(e.g. sqlite amalgamation, git), or `clang` alongside gcc.

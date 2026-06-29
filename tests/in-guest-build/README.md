# in-guest build tier — compile *and* run on the i.MX 95 A55

The strongest self-hosting signal for the board farm: a developer can build their
C code **on the emulated board** and run the result, not just cross-compile it on
a host. (The [`code-sweep`](../code-sweep/) CPU tier cross-compiles on the host
then runs in the guest; this compiles *and* runs entirely in the guest.)

```sh
tests/in-guest-build/run.sh
```

It builds a tiny native toolchain **from source** (nothing is committed as a
binary), boots the busybox initramfs on `imx95-19x19-evk`, compiles every
`tests/*.c` **in-guest** with that toolchain, runs each binary on the A55, and
checks its stdout against the program's own `// EXPECT:` line. Exit 0 iff every
case builds and prints the expected output. SKIPs (exit 0) if a prerequisite
(QEMU, kernel+dtb, SM ELF, cross gcc) is missing.

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

## Adding a test

Drop a `tests/<name>.c` whose **first line** is `// EXPECT: <exact stdout>`. Keep
it to C that musl static + tcc's arm64 codegen support (loops, arithmetic, libc:
`printf`, `qsort`, `malloc`, `string.h`, …). The committed cases exercise loops +
`printf` (`sum.c`), integer arithmetic (`fib.c`), and musl stdlib + function
pointers (`qsort.c`).

## Status / roadmap

MVP proven: the A55 hosts a compiler and runs the binaries it produces
(`pass=3 fail=0`). tcc is the bring-up proof of the *capability*; the heavier,
more representative follow-up is a full **native gcc in a real aarch64 rootfs**
(what farm devs actually use) booted off a disk image — future work.

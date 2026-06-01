# Clean-room reproducibility check

Builds qemu-imx95 and runs the test suite inside a **pristine `ubuntu:22.04`
container**, installing only the packages the top-level README lists. This is
the strongest form of the "fresh-clone, different machine" reproducibility gate
(`docs/imx95/validation-todo.md` Tier 1.1/1.2): a container starts with nothing
pre-installed, so it catches build/runtime dependencies that are invisible on
any machine with prior development work.

It earned its keep on first run — it surfaced three undocumented dependencies
that every prior on-machine test had missed because the packages happened to be
present already:

- `python3-venv` — QEMU's `configure` needs `ensurepip`.
- `python3-tomli` — meson's TOML parser (stdlib only on newer Python).
- `netcat-openbsd` — the M7 boot/fault tests drive QEMU's HMP monitor with
  `nc -U` (Unix socket); the openbsd `nc` is the variant with `-U`.

All three are now in the README's Building section.

## Run

From the repo root:

```sh
tests/docker-repro/run.sh
```

That runs steps 0–3 (prove-pristine, apt install, fresh clone + build, the two
no-artifact smoke tests, and the Tier-1.2 path grep). No NXP artifacts needed.

To also run the full Linux boot to userspace (step 4), point it at a directory
holding the four boot artifacts (not redistributable, so not in the repo):

```sh
IMX95_ARTIFACTS=/path/to/artifacts tests/docker-repro/run.sh
```

where the directory contains `m33_image.elf`, `Image`,
`imx95-19x19-evk.dtb`, and `initramfs.cpio.gz`.

Knobs: `IMAGE=` (base image, default `ubuntu:22.04`), `BRANCH=` (default
`imx95-scaffold`).

## Pass

`>>> ALL PASS (pristine container)` — clean build, both smoke tests, a clean
path grep, and (if artifacts were provided) Linux to userspace. Anything short
of that names a concrete dependency or path gap to fix before submission.

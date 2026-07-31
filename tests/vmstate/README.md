# tests/vmstate

Whole-machine **VMState round-trip** test for `qemu-imx95`. Every device model
declares a `VMStateDescription`, but nothing else in the suite exercises a
`savevm`/`loadvm` (migration) cycle — so a device with a missing field, a wrong
`VMSTATE_*` size, or a `post_load` that fails to rebuild derived state would go
unnoticed until "every snapshot is corrupted." This test closes that gap.

## What it does

`run.sh`:

1. Builds `counter.bin` from `counter.S` — a bare-metal guest that prints a
   one-shot banner, then emits an incrementing letter (`A`, `B`, `C`, …) over
   LPUART1, with the letter index held in a callee-saved register (`x20`).
2. Boots it (**source** QEMU), waits until it is counting.
3. Migrates the whole machine to a file (`migrate exec:cat > state.bin`, driven
   over QMP).
4. Restores that state into a second (**destination**) QEMU with `-incoming`,
   and continues it.
5. Asserts the restored guest **resumed** rather than reset:
   - it does **not** reprint the banner → the CPU **PC** round-tripped;
   - the letter sequence **continues past `A`** → the **GPR** (`x20`) and **RAM**
     round-tripped;
   - migration **completed** (no device aborted serialization).

The Neutron NPU installs a migration blocker only *while an inference is in
flight*; with an idle machine (this test never touches the NPU) the whole SoC
migrates cleanly.

## Run

```sh
tests/vmstate/run.sh
# PASS: machine migrated and the guest RESUMED mid-loop ...
```

Requires the `aarch64-linux-gnu` binutils (same as `tests/hello-imx95`) and a
built `build/qemu-system-aarch64`. No external BSP artifacts. Also runs as part
of `tests/run-all.sh`.

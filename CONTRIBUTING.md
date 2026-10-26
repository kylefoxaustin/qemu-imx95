# Contributing

Pull requests are welcome here, and they get read.

This repository is **not** a read-only mirror of upstream QEMU. It is an active
fork carrying a i.MX 95 machine model that does not exist upstream. If you found
a defect in the model, or you have hardware behaviour we got wrong, open a pull
request or an issue — both reach a human.

If your patch fixes *generic* QEMU code rather than the i.MX 95 model, it belongs
upstream as well as here: send it to qemu-devel following
https://www.qemu.org/contribute/. Merging it here does not land it upstream.

## What gets merged

A patch is easiest to accept when it carries its own evidence.

- **Say what the hardware does, and how you know.** A reference-manual section, a
  trace captured from real silicon, or a guest driver that fails are all good
  grounds — they are simply not the same kind of claim, so label which one you
  have. "The RM says" and "I measured it on the board" carry different weight and
  reviewers need to know which they are reading.
- **One logical change per commit**, subject line prefixed `imx95:` for model
  changes, or the usual subsystem prefix (`hw/net:`, `hw/sd:`) for generic code.
- **Run checkpatch**: `scripts/checkpatch.pl --strict` over your patch.
- **Say what you tested.** Which guest, which kernel, what you saw before and
  after. A model change nothing exercises is hard to review and easy to regress.

## What gets sent back

- **Registers that return plausible-looking invented values.** A window that reads
  as zero and is *declared* a stub is fine and often the right answer. A window
  that makes up values the guest then computes on is a silent wrong answer — the
  failure class this project spends the most effort avoiding. If you need a stub,
  mark it as one.
- **Non-redistributable vendor artifacts.** Never put NXP BSP binaries — kernels,
  device trees, rootfs images, firmware ELFs — in a patch. Build them locally, or
  have the model load a file the operator supplies.
- **Bulk reformatting mixed into a behaviour change.** Split them into separate
  commits.

## Questions

Open an issue. Asking what the model does or does not implement is a perfectly
good use of the tracker — it is usually faster than reading the source, and the
answer helps the next person.

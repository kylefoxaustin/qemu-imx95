# i.MX 95 / 952: System Manager, Messaging Unit & SCMI bring-up — the hard-won map

A write-up for whoever is bringing up an i.MX 95-family QEMU machine (incl. the
952). This is NOT a code tour — read the code for that. This is the **connected
story** behind the low-level bring-up, and the traps you WILL hit, in the order
you'll hit them, because they're one system, not N independent bugs. The single
banner/doorbell symptom you may be chasing is the visible tip of this.

── The one mental model everything hangs off ─────────────────────────────────

On the i.MX 95 the **System Manager (SM) firmware runs on the Cortex-M33 and is
the ONLY SCMI provider.** There is no SCMI implementation in QEMU. Linux's clock,
power, reset, perf, and sensor ops are all *served by real NXP firmware* running
on the emulated M33, over a shared Messaging Unit (MU2) mailbox. (Contrast the
i.MX 93, which has NO SM — Linux pokes CCM/ANATOP/SRC directly. Do not port 93's
"model the clock controller" instinct to a 95; the 95's answer is "make the
mailbox to the real firmware work.")

Consequence: **booting Linux requires the real SM firmware loaded on the M33.**
Everything below exists to make that firmware run and its mailbox handshake with
Linux/U-Boot. If your banner/doorbell is silent, you are almost always looking at
one link in the A55↔M33 mailbox chain.

── The Messaging Unit model — where doorbells live ───────────────────────────

The MU (V2 register layout) is the channel. Key registers: CR/SR, GCR/GSR/GIER
(general-purpose / doorbell), TCR/TSR/RCR/RSR (transmit/receive control+status),
and 4 TR/RR data registers. Two independent signalling paths ride it:

1. **Doorbells (the thing that wasn't ringing).** A writer sets `GCR.GIRn`. The
   model must **detect the 0→1 transition** on that bit and, for each newly-
   asserted channel, latch `GSR.GIPn` on the *peer* MU — which raises the peer's
   IRQ *once the peer has enabled `GIER.GIEn`*. Three ways this silently fails:
   - You're edge-missing: latching on level, not the 0→1 edge, so a re-arm never
     re-fires (or a stale bit fires forever).
   - The peer link isn't set, so `GCR.GIRn` writes go into the void (see below).
   - The peer hasn't set `GIER.GIEn` yet, so `GSR.GIPn` is latched but no IRQ is
     raised. A doorbell that arrives *before* the receiver enables its interrupt
     must still be pending when it does — model GIP as latched state, not a pulse.

2. **Data + TX/RX handshake (TR/RR).** Writing `TR[n]` clears `TSR.TEn`
   (TX-not-empty). For a **peer-linked** MU the word is delivered into the peer's
   `RR[n]` and the peer's RX status/IRQ is raised. Crucially, **`TEn` stays clear
   until the peer READS `RR[n]`** — reading RR is what re-asserts the sender's
   TX-empty and lets a blocked `mbox` send complete. This IS the real MU full/
   empty handshake the `imx-mailbox` driver waits on. Get the "TEn clears on TR
   write, re-asserts on peer RR read" cycle wrong and SCMI hangs mid-transaction
   with no error — just silence.

**Peer wiring.** The two endpoints of one physical MU (MUA on the A55 side, MUB
on the M33/SM side) are linked so a doorbell/TR on one side lands on the other.
In this tree that's a QOM link property ("peer"). Two gotchas that cost real
time: `object_property_add_link()` with a NULL check is **read-only** — pass a
(no-op) check fn to make it settable; and the SM-side MUB is created *after* the
M33 is realized, so the peer is linked **post-realize** (a static DEFINE_PROP_LINK
forbids that — hence add_link + a permissive check). If your doorbell goes
nowhere, first assert the peer pointer is actually set on both sides.

**Delivery modes.** There are two: (a) **peer-linked** (real MUA↔MUB cross-
connect — this is how the real SM firmware answers), and (b) a **handler
callback** (a C-stub responder for when there's no firmware on the far side).
The 95/952 SM path uses (a). If you're stubbing early bring-up you may lean on
(b); just know the real path is the peer link, and a "NOP tr-write handler whose
mere presence flips the model into setting TSR.TEn" is a bring-up crutch, not the
final design.

── The traps, in the order you'll hit them ──────────────────────────────────

Once the doorbell rings, the boot proceeds and hits these — each has bitten this
port, and they LOOK unrelated but are all "the SM/M-core lifecycle":

1. **`cpuidle.off=1` is not optional (yet).** When Linux idles the CPU that SCMI
   IRQs target, cpuidle disables the GIC CPU interface (`ICC_IGRPEN1=0`), and
   QEMU can't wake it — SCMI stalls, boot hangs. Booting with `cpuidle.off=1`
   sidesteps it; the proper fix is SM CPU-power modelling. If Linux hangs *after*
   SCMI was working, suspect this before you suspect the MU.

2. **Gate the M33 on its firmware.** Load the SM as an **ELF, not a .bin**, and
   don't let the M33 run until its ITCM firmware is actually present — gate the
   M-core start on a machine-done bottom-half. A M33 that runs before its vectors
   are loaded faults instantly and nothing answers SCMI.

3. **M33 idle-spin / board-farm density.** Root-caused to an upstream ARM
   M-profile bug: `arm_cpu_has_work()` lets a WFE event-register gate WFI, so the
   SM never actually halts and spins ~90% host CPU. Fix = a wfe_halt flag so WFI
   halts properly. Symptom is "everything works but one QEMU pins a core" — costs
   you nothing functionally, kills you at scale.

4. **ELE responder.** U-Boot/SPL and the SM issue EdgeLock Enclave requests
   (GET_INFO, get-random, …) over a dedicated MU before SCMI is fully up. Model a
   responder that consumes the request words and pushes a reply into RR, or early
   bring-up wedges waiting on ELE. (U-Boot proper also hard-codes an ELE mailbox
   base — mirror the ELE responder there too.)

5. **Byte-width MMIO.** eDMA and `memset_io`-style accesses hit MU/peripheral
   registers at 1/2/8-byte widths. A model with `.valid.min_access_size = 4`
   silently DROPS those writes (this bit the LPSPI TDR and the FlexCAN mailboxes
   too). If a register "isn't taking the write," check the access-size guard
   before you doubt your logic.

── How to debug this class (the SM fw has no gdb) ───────────────────────────

You cannot gdb the SM firmware. So:
 - **Instrument the MU path, not the guest.** Add trace on TR write / RR deliver /
   doorbell (GIR) / GIP assert. There's an `imx_mu_scmi_request` trace event that
   captured a **golden SCMI trace** (~1800 requests / 67 req-reply pairs: 175
   clocks, 75 pins, 23 power domains …). Diff your run against that golden trace —
   the first divergence is your bug.
 - **A spinning vCPU is invisible to guest tooling.** Measure HOST per-thread CPU
   to see the M33 spinning; the guest will look "fine."
 - **Per-event `qemu_log` throttles and lies** under a storm — batch or count.
 - **"A wall is a hypothesis."** A silent hang in SCMI is almost always one of:
   peer not linked → doorbell nowhere; GIEn not set → IRQ never raised; TEn/RR
   handshake wrong → mbox send never completes; or cpuidle killed the GIC cpuif.
   Check them in that order.

── 952-specific ─────────────────────────────────────────────────────────────

The 952 is a 95 with deltas. Watch for the **TRDC BusFault** on early access
(the trusted resource-domain controller faults if a region isn't permitted before
the firmware touches it) and port the handful of 95 fidelity fixes (the MU byte-
MMIO, the ELE mirror, the wfe_halt, the SD/uSDHC quirks) rather than re-deriving
them. The SM/MU/SCMI machinery is the SAME — if it boots on the 95 it boots on the
952 once the memory map and TRDC permissions match.

────────────────────────────────────────────────────────────────────────────
The through-line: the banner is a symptom of the mailbox, the mailbox is the
whole boot, and these "separate" bugs are one lifecycle. Solve them as a system.
Ping @95emulator on the bus (or Kyle) if a link in the chain fights you — I've
walked every one of these.

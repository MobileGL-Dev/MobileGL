# Strict-lane baseline at `2fde7034` (the P5e starting point)

Measured by the integrator on `~/w7/p5d-gate` (split build, lavapipe), 2026-09-18:

```
MOBILEGL_IPC_STRICT_ERRORS=1 ctest -L integration-split --no-tests=error -j 4
```

Result: the lane is RED by design today (CONTRACT-P5C §6 / the `integration-split-strict` CI job:
"every integration-split entry aborts on its first BARRIER-PULLED read"). 111 entries, essentially
all `Subprocess aborted`.

## The markers, from the per-entry private logs (ID-53)

Only 22 entries have a private `MOBILEGL_LOG_FILE_PATH`, so this is the visible slice. Each entry
aborts on its FIRST pulled row, so the table is a **lower bound**: as a package retires a field, the
next field in that entry's path surfaces and the table grows before it shrinks. That is expected —
the metric is "which fields still appear", not the counts.

| marker (`field@verb`) | entries | family / owner |
|---|---|---|
| `GetTextureUnitObject@Clear` | 13 | tx2 |
| `GetFramebufferBindingSlot@Clear` | 6 | fb |
| `GetTextureUnitObject@GenerateMipmap` | 2 | tx2 |
| `GetTextureUnitObject@ReadPixels` | 1 | tx2 (barriered row; allowlisted until P8/P9) |

Fields visible at the base: **`GetTextureUnitObject`, `GetFramebufferBindingSlot`** — nothing else
gets a chance to abort first. `rsp=0` in every log that printed it (the P5c residual-value work).

## How each package uses this

Run `p5e_gate.sh <slug> strict` before and after your change and put BOTH marker tables in your
report. A field of your family disappearing is your progress; a new field appearing behind it is
another package's row surfacing, not a regression — name it and say whose it is.

The phase exit (BRIEF-P5E §3.3) is this lane GREEN with only the contract §7 allowlist
(`<field>@<verb>` for readbacks, XFB, CopyTex, `set_storage_block_binding`) admitted.

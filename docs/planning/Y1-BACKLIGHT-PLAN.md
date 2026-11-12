# Y1 backlight: brightness and screen-off through the MT6323 current sinks

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: **PASS** on the owner's player (2026-11-05, [run](#7-run--pass--2026-11-05)); approved by the
owner with both admission items the same day. Follows the
[display plan](Y1-DISPLAY-PLAN.md) (stage B PASS: Linux's DRM pipeline drives the panel; the backlight is
still as LK set it).

## 1. Goal

Linux sets the Y1's screen brightness and can switch the backlight off and on, through the standard
`/sys/class/backlight` interface, never brighter than LK leaves it.

Not in scope: changing the LED current, panel power or blanking (the display keeps scanning out), a
brightness policy or user interface, suspend.

## 2. Evidence

- **Stock 3.0.2** (display stage 0, `evidence-private/2026-11-05-y1-display-a/stage0-notes.md`): the
  `lcd-backlight` entry of the LED table is MT6323 PMIC mode, `data 2` = **ISINK0 and ISINK1 in
  parallel** (`brightness_set_pmic`); the panel has no backlight hook; BLS PWM and `Cust_SetBacklight`
  are unused. Per level stock writes the duty (`0x330`/`0x338` bits 12:8, 0–31), the step (bits 14:12 of
  `0x334`/`0x33c`), `fsel` 2, and the enables (`0x356` bits 0/1); level 0 clears the enables. CONFIRMED.
- **Live, after LK** (display stage A): `0x330`/`0x338` = `0x1f00` (duty 31, PWM mode 0), `0x334`/`0x33c` =
  `0x4000` (step 4), `0x340`/`0x348` = `0x0f08` (ISINK2/3 configured but not enabled), `0x350` = `0x0800`,
  `0x354` = `0x0003`, `0x356` = `0x0033` (ISINK0/1 on, chop 0/1 on).
- **The Y2's driver** (`kernel/platform/backlight.c`, physically qualified on the Y2): reads the inherited
  sinks at probe and writes nothing then; brightness 1–32 sets only the duty field, never above the lowest
  inherited duty; 0 clears the enables; the current step, clock, mode and chop stay as inherited; the
  sinks go dark during a change and on shutdown; a failed write rolls back and stays dark. It requires
  four active sinks (the Y2's panel).

## 3. Design

- The Y2 driver, shared, gains `innioasis,isink-mask` (default `0xf`, the Y2 unchanged): the Y1 tree sets
  `0x3`, so it checks, reads and writes only ISINK0 and ISINK1 and enable bits 0/1. Its Y2 power-management
  calls are inline no-ops without `CONFIG_Y2_POWER`.
- MFD cell for `innioasis,y2-backlight` in the Y1's `mt6397-core.c` overlay (`0014`), as the Y2's `0024`.
- **Write policy:** the PMIC wrapper's Y1 filter (overlay `0005`) extends from the charger block to the
  ISINK block: writes to `0x330`–`0x356` must pass the Y2's `y2_pmic_write_mask` (`0x330`/`0x338`/
  `0x340`/`0x348`: duty bits 12:8 only; `0x356`: enables bits 0–3 only) and may not change anything else.
  The current step, `0x350`–`0x354`, the clocks and the chop bits stay unwritable.
- **Test interface:** the USB shell gets its first write command, `bl N`, which writes `N` to the one
  backlight's `brightness` file and nothing else. The status line shows the brightness.

## 4. Test

Owner run: `bl 32` (LK's level), `bl 16`, `bl 4`, `bl 1`, `bl 0` (dark: the picture is still there,
visible under a torch), `bl 32` again; the owner reports what the screen does; the PMIC registers are read
back after each step (`pmic`-style read of `0x330`–`0x356`).

## 5. Admission

| Item | Bound |
| --- | --- |
| **Write the MT6323 ISINK0/1 duty and enable bits** | duty never above LK's (31), current step, clock, mode and chop untouched; ISINK2/3 never written; the wrapper refuses anything else in `0x330`–`0x356` |
| **A shell command that writes** | `bl N` only: the backlight `brightness` file; no other write command |

## 6. Build — 2026-11-05

- `kernel/platform/backlight.c` (shared): `innioasis,isink-mask` (default `15`, the Y2 unchanged) selects
  the sinks for the probe checks, duty writes, enables, rollback, `get_brightness` and shutdown;
  `y2_backlight_dark` is built only with `CONFIG_Y2_POWER` (its only readers). `test_display_quiet_power`
  now checks the mask-based shutdown and the default of 15 instead of the literal `15`;
  `test_cpu_fix02`/`03` pass unchanged.
- Overlay `0014` adds the `y2-backlight` MFD cell; `0005`'s policy covers `0x330`–`0x356` with the Y2's
  masks; `0026`/`0027` add `BACKLIGHT_INNIOASIS_MT6323` (added files `innioasis-mt6323-backlight.c`,
  `system-idle.h`).
- `innioasis-y1-eighth-boot.dts` (the seventh tree plus `backlight` with `innioasis,isink-mask = <0x3>`),
  `y1-eighth-boot.config` (`BACKLIGHT_CLASS_DEVICE`, the driver), PID1 `bl [N]`
  (`initramfs/y1-eighth-boot.h`).

Build `out/y1-eighth-boot-2`, BOOTIMG sha256
`e319eeaaf6782a8104278e0a7cdf0cbf5b266fd3dc05b6658be9e5739e783668`, no compiler warnings. Physical status
NOT_RUN.

## 7. Run — PASS — 2026-11-05

Build `e319eeaa…`; `evidence-private/2026-11-05-y1-backlight/`. The driver adopted LK's state without a
write (`brightness=32 max_brightness=32`, registers unchanged). Over the USB shell `bl` stepped
32, 16, 4, 1, 0, 32 (8 s each); after each step the ISINK block read back:

| Level | `0x330`/`0x338` | `0x356` | `0x334`/`0x33c` |
| --- | --- | --- | --- |
| 32 | `0x1f00` | `0x0033` | `0x4000` |
| 16 | `0x0f00` | `0x0033` | `0x4000` |
| 4 | `0x0300` | `0x0033` | `0x4000` |
| 1 | `0x0000` | `0x0033` | `0x4000` |
| 0 | `0x0000` | `0x0030` | `0x4000` |
| 32 | `0x1f00` | `0x0033` | `0x4000` |

Only the duty fields and enable bits 0/1 changed; the current step, `0x350`/`0x354`, ISINK2/3 and the chop
bits stayed as LK left them. `0x352` moved with the duty (`0x000c`, `0x0004`, `0`) without any write (the
policy refuses it): a status field, INFERRED. The owner: the screen dimmed step by step, went dark at 0
and came back bright at 32, with no flicker. **Backlight: PASS.**

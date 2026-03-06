# Release notes — Backlight workaround

Temporary firmware-only workaround until the kernel driver is updated. Build with `-DBACKLIGHT_IGNORE_HOST=ON -DBACKLIGHT_PERSIST=ON` (e.g. use `./build_backlight_fw.sh`).

## Key combos

| Combo | Action |
|---|---|
| **Sym or Alt + key with 0** | Toggle backlight on/off |
| **Sym + Shift + digits 1–9** | Set brightness level (1 = dimmest, 9 = full) |

Brightness combo keys are fully consumed by the firmware — no keypresses are sent to the OS.

## Persistence

- The current on/off state **and** the last brightness level are stored separately in the RP2040's internal flash.
- State is tracked in RAM for instant reads and debounced (2 s) to flash for power-cycle persistence.
- Setting a brightness level (Sym + Shift + digit) saves both the on state and the level.
- Toggling off (Sym + 0) saves "off" but preserves the last brightness, so toggling back on restores it.

## Host interaction (lock / unlock)

- **Lock (host sends 0):** The firmware applies it so the backlight turns off visually. This is **not** saved to state — your last user choice is preserved.
- **Unlock (host sends non-zero):** The firmware ignores the host value and checks the persisted state. If you last chose "on," it restores that brightness. If you last chose "off," it does nothing. The host cannot force the backlight on.
- **Cooldown:** For 500 ms after a key combo change, all host backlight writes are ignored to prevent delayed lock/unlock packets from overwriting the combo action.

## Removing the workaround

When the kernel driver is fixed (e.g. no backlight write on unlock), build without `BACKLIGHT_IGNORE_HOST` to restore normal host control.

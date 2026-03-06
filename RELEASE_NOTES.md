# Release notes — Backlight workaround

Temporary firmware-only workaround until the kernel driver is updated. Build with `-DBACKLIGHT_IGNORE_HOST=ON -DBACKLIGHT_PERSIST=ON` (e.g. use `./build_backlight_fw.sh`).

## Behavior

- **Key combo:** Backlight is toggled only by **Sym or Alt + the key with 0** (firmware sees `~` or `0`). No other host backlight writes can turn the light on.
- **Persistence:** The last level you set with the key combo is saved in the RP2040’s internal flash and restored on power-up.
- **Lock:** When the host turns the backlight off (e.g. on screen lock), the firmware applies it so the light turns off. That “off” is **not** saved, so the stored “last user level” stays unchanged.
- **Unlock:** When the host sends “turn on,” the firmware ignores the host value and instead reads the persisted level. If the last user state was on, it restores that level; if it was off, it does nothing. So the backlight matches your last choice after unlock.
- **No auto-on on unlock:** The host cannot force the backlight on; only the key combo or the restored persisted state can turn it on.

When the kernel driver is fixed (e.g. no backlight write on unlock), build without `BACKLIGHT_IGNORE_HOST` to restore normal host control.

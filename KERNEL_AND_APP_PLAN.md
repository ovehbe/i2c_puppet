# Kernel & App Backlight Implementation Plan

This document describes the exact kernel driver modifications and companion app needed to give userspace full control over the BBQ20 keyboard backlight — including software toggle, brightness adjustment, and optional auto-brightness via a luminance sensor.

## Prerequisites (already done)

The i2c_puppet firmware has already been rebuilt **without** `BACKLIGHT_IGNORE_HOST`, so the firmware now accepts all host writes to `REG_BKL`. It is built with `BACKLIGHT_PERSIST=ON` only, meaning:

- Host (kernel) writes to `REG_BKL` are accepted and applied immediately
- Firmware persists the last brightness level to flash across power cycles
- Physical key combos on the keyboard still work (Sym+0 toggle, Sym+Shift+1–9 levels)

Firmware backups are at `firmware-backups/` in the firmware repo:
- `i2c_puppet_backlight_workaround_*.uf2` — old firmware with `BACKLIGHT_IGNORE_HOST` (rollback if needed)
- `i2c_puppet_persist_only_*.uf2` — current firmware, accepts host writes

**Firmware repo**: `https://github.com/ovehbe/i2c_puppet`

## Context

- **Kernel source**: `/home/ovehbe/Code/q25-kernel-source`
- **Driver path**: `drivers/input/keyboard/bbqX0kbd/`
- **I2C address**: `0x1F`
- **Backlight register**: `REG_BKL` (`0x05`), write mask `0x80`, range 0–255

## Part 1: Kernel Driver Changes

All changes are in `drivers/input/keyboard/bbqX0kbd/`.

### 1.1 Register a Linux backlight device

The driver already includes `<linux/backlight.h>` (line 9 of `bbqX0kbd_main.h`) but never uses it. Register a `backlight_device` so the standard sysfs interface appears at `/sys/class/backlight/bbq20kbd-backlight/`.

#### Files to modify

**`bbqX0kbd_main.h`** — add a backlight_device pointer to the data struct:

```c
// In struct bbqX0kbd_data, after the existing members:
struct backlight_device *backlight_dev;
```

**`bbqX0kbd_main.c`** — add backlight ops and register in probe, unregister in remove:

```c
/* --- Backlight subsystem integration --- */

static int bbqX0kbd_bl_update_status(struct backlight_device *bl)
{
    struct bbqX0kbd_data *data = bl_get_data(bl);
    int brightness = backlight_get_brightness(bl);

    data->keyboardBrightness = (uint8_t)brightness;
    return bbqX0kbd_write(data->i2c_client, BBQX0KBD_I2C_ADDRESS,
                          REG_BKL, &data->keyboardBrightness, sizeof(uint8_t));
}

static int bbqX0kbd_bl_get_brightness(struct backlight_device *bl)
{
    struct bbqX0kbd_data *data = bl_get_data(bl);
    uint8_t val = 0;
    int ret;

    ret = bbqX0kbd_read(data->i2c_client, BBQX0KBD_I2C_ADDRESS,
                        REG_BKL, &val, sizeof(uint8_t));
    if (ret)
        return ret;

    data->keyboardBrightness = val;
    return val;
}

static const struct backlight_ops bbqX0kbd_bl_ops = {
    .update_status  = bbqX0kbd_bl_update_status,
    .get_brightness = bbqX0kbd_bl_get_brightness,
};
```

#### In `bbqX0kbd_probe()` — register the backlight device

Add this **after** the input device is registered (after `input_register_device`), around line 1090:

```c
/* Register keyboard backlight with Linux backlight subsystem */
{
    struct backlight_properties bl_props;
    memset(&bl_props, 0, sizeof(bl_props));
    bl_props.type = BACKLIGHT_RAW;
    bl_props.max_brightness = 255;
    bl_props.brightness = bbqX0kbd_data->keyboardBrightness;

    bbqX0kbd_data->backlight_dev = backlight_device_register(
        "bbq20kbd-backlight", dev, bbqX0kbd_data,
        &bbqX0kbd_bl_ops, &bl_props);
    if (IS_ERR(bbqX0kbd_data->backlight_dev)) {
        dev_warn(dev, "Failed to register backlight: %ld\n",
                 PTR_ERR(bbqX0kbd_data->backlight_dev));
        bbqX0kbd_data->backlight_dev = NULL;
    }
}
```

#### In `bbqX0kbd_remove()` or driver cleanup — unregister

```c
if (bbqX0kbd_data->backlight_dev)
    backlight_device_unregister(bbqX0kbd_data->backlight_dev);
```

> **Note**: The current driver uses `devm_` allocation for the input device, but `backlight_device_register` is not devm-managed. You can alternatively use `devm_backlight_device_register()` (available in newer kernels) to avoid needing explicit unregister.

### 1.2 Fix suspend/resume to respect last brightness

This is the critical bug: `bbqX0kbd_resume()` currently hardcodes `keyboardBrightness = 0xFF` (line 1222). It should restore the last-set brightness instead.

**In `bbqX0kbd_suspend()`** (line 1171) — no changes needed. It already writes 0 to `REG_BKL` to turn off the backlight. The `bbqX0kbd_data->keyboardBrightness` field is NOT updated in suspend, so it still holds the last brightness.

**In `bbqX0kbd_resume()`** (line 1200) — **delete line 1222**:

```c
/* DELETE this line: */
bbqX0kbd_data->keyboardBrightness = 0xFF;
```

The field already holds the last-set value from before suspend, and the write on line 1223 will restore it correctly.

Also update the backlight device if it exists, so sysfs stays in sync. Add after the `REG_BKL` write in resume:

```c
if (bbqX0kbd_data->backlight_dev)
    backlight_force_update(bbqX0kbd_data->backlight_dev, BACKLIGHT_UPDATE_HOTKEY);
```

### 1.3 Sync key combo brightness changes with backlight device

The existing `bbqX0kbd_set_brightness()` function (line 229) handles Right Alt + Z/X/0 key combos. After it writes to `REG_BKL`, it should also notify the backlight subsystem so `/sys/class/backlight/` stays in sync:

```c
/* After the existing bbqX0kbd_write() call for REG_BKL in bbqX0kbd_set_brightness(): */
if (bbqX0kbd_data->backlight_dev) {
    bbqX0kbd_data->backlight_dev->props.brightness = bbqX0kbd_data->keyboardBrightness;
    backlight_force_update(bbqX0kbd_data->backlight_dev, BACKLIGHT_UPDATE_HOTKEY);
}
```

### 1.4 Summary of kernel changes

| File | Change | Lines |
|------|--------|-------|
| `bbqX0kbd_main.h` | Add `struct backlight_device *backlight_dev` to `bbqX0kbd_data` | ~after line 97 |
| `bbqX0kbd_main.c` | Add `bbqX0kbd_bl_update_status`, `bbqX0kbd_bl_get_brightness`, `bbqX0kbd_bl_ops` | new, before `probe()` |
| `bbqX0kbd_main.c` | Register backlight in `bbqX0kbd_probe()` | ~line 1090 |
| `bbqX0kbd_main.c` | Unregister backlight in cleanup/remove | end of remove |
| `bbqX0kbd_main.c` | **Delete** `bbqX0kbd_data->keyboardBrightness = 0xFF;` in `bbqX0kbd_resume()` | line 1222 |
| `bbqX0kbd_main.c` | Sync backlight device in `bbqX0kbd_resume()` after REG_BKL write | ~line 1226 |
| `bbqX0kbd_main.c` | Sync backlight device in `bbqX0kbd_set_brightness()` after REG_BKL write | ~line 281 |

### 1.5 Safe testing via module hot-swap (no kernel flash needed)

The driver is compiled as a loadable module (`CONFIG_KEYBOARD_BBQX0_KEYBOARD=m` in `arch/arm64/configs/q20_v12_factory.config`). The stock `.ko` is packed inside the `vendor_boot` image (not on the regular filesystem), but it can still be swapped at runtime via `rmmod`/`insmod`.

#### Step 1: Build the stock (unmodified) module first

Before making any changes, cross-compile the **original** module so you have a known-good backup to swap back to without rebooting:

```bash
cd /home/ovehbe/Code/q25-kernel-source
# Use the same cross-compiler and defconfig as a normal kernel build
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules M=drivers/input/keyboard/bbqX0kbd
```

Save the original:

```bash
mkdir -p /home/ovehbe/Code/q25-kernel-source/module-backups
cp drivers/input/keyboard/bbqX0kbd/bbqX0kbd.ko module-backups/bbqX0kbd_stock.ko
```

#### Step 2: Build the modified module

After applying the changes from sections 1.1–1.4, rebuild:

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules M=drivers/input/keyboard/bbqX0kbd
cp drivers/input/keyboard/bbqX0kbd/bbqX0kbd.ko module-backups/bbqX0kbd_backlight.ko
```

Now you have both modules:
- `module-backups/bbqX0kbd_stock.ko` — original, no backlight sysfs
- `module-backups/bbqX0kbd_backlight.ko` — modified, with backlight sysfs + resume fix

#### Step 3: Push both to the phone

```bash
adb push module-backups/bbqX0kbd_stock.ko /data/local/tmp/bbqX0kbd_stock.ko
adb push module-backups/bbqX0kbd_backlight.ko /data/local/tmp/bbqX0kbd_backlight.ko
```

#### Step 4: Swap to the modified module

```bash
# Keyboard will briefly stop working for ~2 seconds
adb shell su -c "rmmod bbqX0kbd"
adb shell su -c "insmod /data/local/tmp/bbqX0kbd_backlight.ko"
```

#### Step 5: Verify

```bash
# Check if the backlight sysfs node appeared
adb shell ls /sys/class/backlight/bbq20kbd-backlight/

# Try setting brightness
adb shell su -c "echo 128 > /sys/class/backlight/bbq20kbd-backlight/brightness"

# Read it back
adb shell cat /sys/class/backlight/bbq20kbd-backlight/brightness
```

#### Swap back to stock (no reboot needed)

```bash
adb shell su -c "rmmod bbqX0kbd"
adb shell su -c "insmod /data/local/tmp/bbqX0kbd_stock.ko"
```

#### If anything goes wrong

Just reboot. The phone loads the original module from `vendor_boot` on every boot. Nothing on disk is modified.

### 1.6 Permanent deployment via KernelSU module

Once the modified module is tested and working, you can make it survive reboots without reflashing the kernel by packaging it as a **KernelSU module**. The stock `.ko` lives inside the `vendor_boot` image (not on a regular filesystem path), so the systemless overlay approach won't work. Instead, use a boot script that swaps the module after the stock one loads.

**Create the KernelSU module structure:**

```
q25-kbd-backlight/
├── module.prop
├── post-fs-data.sh
├── bbqX0kbd_backlight.ko   # modified module
└── bbqX0kbd_stock.ko       # original module (for rollback)
```

**`module.prop`:**

```properties
id=q25-kbd-backlight
name=Q25 Keyboard Backlight Control
version=1.0
versionCode=1
author=ovehbe
description=Replaces bbqX0kbd module with backlight sysfs support and resume fix
```

**`post-fs-data.sh`** (swaps the module on every boot):

```bash
#!/system/bin/sh
MODDIR="${0%/*}"
sleep 2
rmmod bbqX0kbd 2>/dev/null
insmod "$MODDIR/bbqX0kbd_backlight.ko"
```

**Package and install:**

```bash
cd q25-kbd-backlight
zip -r ../q25-kbd-backlight-v1.0.zip .

# Install via KernelSU Manager app on the phone, or:
adb push ../q25-kbd-backlight-v1.0.zip /sdcard/
# Then install from KernelSU Manager → Modules → Install from storage
```

**Benefits:**
- Survives reboots automatically
- Survives OTA updates (reapplied on every boot)
- Completely reversible — disable or uninstall from KernelSU Manager to revert to stock
- Both modules are bundled so the app (see Part 2) can switch between them

### 1.7 Future: app-managed module switching

The companion Android app (Part 2) can bundle both `.ko` files in its assets and manage the swap itself, with a root shell. This would let the user:

- **Enable** backlight control: app runs `rmmod bbqX0kbd && insmod bbqX0kbd_backlight.ko`
- **Disable** backlight control: app runs `rmmod bbqX0kbd && insmod bbqX0kbd_stock.ko`
- **Auto-load on boot**: app registers a boot receiver that runs the swap script (same as the KernelSU module but managed from the app UI)

This means rooted users wouldn't need KernelSU modules at all — the app handles everything, including rollback, from a single toggle in the UI.

### 1.8 Result after kernel changes

Once the kernel is rebuilt and booted:

```
/sys/class/backlight/bbq20kbd-backlight/brightness      # read/write 0–255
/sys/class/backlight/bbq20kbd-backlight/max_brightness   # read-only, 255
/sys/class/backlight/bbq20kbd-backlight/actual_brightness # read-only (from get_brightness)
```

Any process can:
```bash
# Read current brightness
cat /sys/class/backlight/bbq20kbd-backlight/brightness

# Set brightness to 128
echo 128 > /sys/class/backlight/bbq20kbd-backlight/brightness

# Turn off
echo 0 > /sys/class/backlight/bbq20kbd-backlight/brightness
```

---

## Part 2: Android app for auto-brightness

### 2.1 Architecture

```
┌─────────────────────────────┐
│       Android App           │
│  ┌───────────────────────┐  │
│  │  Luminance Sensor     │  │  ← android.hardware.Sensor.TYPE_LIGHT
│  │  Listener             │  │
│  └──────────┬────────────┘  │
│             │ lux value     │
│  ┌──────────▼────────────┐  │
│  │  Brightness Curve     │  │  ← maps lux → 0–255
│  │  (configurable)       │  │
│  └──────────┬────────────┘  │
│             │ brightness    │
│  ┌──────────▼────────────┐  │
│  │  Write to sysfs       │  │  ← /sys/class/backlight/bbq20kbd-backlight/brightness
│  │  (needs root or       │  │
│  │   udev/SELinux rule)  │  │
│  └───────────────────────┘  │
└─────────────────────────────┘
```

### 2.2 Sysfs access from Android

The app needs to write to `/sys/class/backlight/bbq20kbd-backlight/brightness`. Options:

1. **Root access** (simplest for a custom phone): the app runs `su -c "echo VALUE > /sys/class/backlight/..."` or opens the file directly with root.

2. **SELinux policy** (production-quality): add a sepolicy rule that allows a specific app/service to write to the backlight sysfs node. This is the Android-native approach.

3. **udev/permissions rule** (if running a Linux userspace instead of Android): set the sysfs node to world-writable or group-writable for a specific group.

4. **HAL integration** (advanced): write a hardware abstraction layer that Android's `BrightnessService` talks to. This is the most work but makes it appear as a native Android setting.

### 2.3 App features

A minimal app would have:

- **Manual brightness slider** (0–255): writes directly to sysfs
- **Toggle on/off button**: writes 0 or last-set value
- **Auto-brightness checkbox**: when enabled, registers a `SensorEventListener` for `TYPE_LIGHT`, maps lux to brightness via a curve, and writes to sysfs on each sensor event
- **Brightness curve editor** (optional): let the user define min/max brightness and the lux thresholds

### 2.4 Prototype: shell script

Before building a full app, you can test the entire pipeline with a shell script:

```bash
#!/system/bin/sh
# Auto-brightness prototype for Q25 keyboard backlight
# Requires: root, light sensor accessible via sysfs

BKL="/sys/class/backlight/bbq20kbd-backlight/brightness"
# Find light sensor input (path varies by device)
LIGHT_SENSOR="/sys/bus/iio/devices/iio:device0/in_illuminance_raw"

while true; do
    lux=$(cat "$LIGHT_SENSOR" 2>/dev/null || echo 500)

    # Simple curve: dark=full brightness, bright=dim
    if [ "$lux" -lt 10 ]; then
        level=255
    elif [ "$lux" -lt 100 ]; then
        level=180
    elif [ "$lux" -lt 500 ]; then
        level=100
    elif [ "$lux" -lt 1000 ]; then
        level=50
    else
        level=20
    fi

    echo "$level" > "$BKL"
    sleep 2
done
```

### 2.5 App implementation notes

For a proper Android app (Kotlin):

```kotlin
// Reading light sensor
val sensorManager = getSystemService(SENSOR_SERVICE) as SensorManager
val lightSensor = sensorManager.getDefaultSensor(Sensor.TYPE_LIGHT)

sensorManager.registerListener(object : SensorEventListener {
    override fun onSensorChanged(event: SensorEvent) {
        val lux = event.values[0]
        val brightness = luxToBrightness(lux)  // your curve function
        writeBrightness(brightness)
    }
    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
}, lightSensor, SensorManager.SENSOR_DELAY_NORMAL)

// Writing brightness (needs root)
fun writeBrightness(value: Int) {
    val clamped = value.coerceIn(0, 255)
    File("/sys/class/backlight/bbq20kbd-backlight/brightness")
        .writeText(clamped.toString())
}
```

---

## Part 3: Implementation order

1. **Cross-compile stock module** — build the unmodified driver to have a known-good backup `.ko`
2. **Kernel: register backlight device** — this is the foundation everything else depends on
3. **Kernel: fix resume** — delete the hardcoded `0xFF`, let it restore last brightness
4. **Kernel: sync key combos** — make sysfs stay in sync with physical key combo changes
5. **Cross-compile modified module** — build the modified driver as `bbqX0kbd_backlight.ko`
6. **Test via hot-swap** — push both `.ko` files, `rmmod`/`insmod` the modified one, verify sysfs works, swap back to stock if needed
7. **Flash the new firmware** (persist-only, no `BACKLIGHT_IGNORE_HOST`) — `firmware-backups/i2c_puppet_persist_only_*.uf2`
8. **Deploy permanently** — package as KernelSU module or let the app manage it
9. **App: manual control** — slider + toggle + module switching (stock/backlight) for rooted users
10. **App: auto-brightness** — light sensor + curve + periodic sysfs writes

---

## Part 4: Key reference — existing driver code locations

| What | File | Line(s) |
|------|------|---------|
| `#include <linux/backlight.h>` (already present) | `bbqX0kbd_main.h` | 9 |
| `keyboardBrightness` field | `bbqX0kbd_main.h` | 96 |
| `lastKeyboardBrightness` field | `bbqX0kbd_main.h` | 97 |
| Key combo brightness (RAlt+Z/X/0) | `bbqX0kbd_main.c` | 229–287 |
| Write to `REG_BKL` after key combo | `bbqX0kbd_main.c` | 281 |
| Probe — initial brightness = 0xFF | `bbqX0kbd_main.c` | ~1090 (look for `REG_BKL` write) |
| Shutdown — backlight off | `bbqX0kbd_main.c` | 1142–1168 |
| Suspend — backlight off | `bbqX0kbd_main.c` | 1171–1198 |
| Resume — **hardcodes 0xFF** (the bug) | `bbqX0kbd_main.c` | 1222 |
| Resume — writes to REG_BKL | `bbqX0kbd_main.c` | 1223 |
| Display notifier (lock/unlock) | `bbqX0kbd_main.c` | 1237–1263 |
| `REG_BKL` definition | `bbqX0kbd_registers.h` | 55 |
| `BBQX0KBD_WRITE_MASK` (0x80) | `bbqX0kbd_registers.h` | 15 |
| `BBQ10_BRIGHTNESS_DELTA` (16) | `bbqX0kbd_registers.h` | 93 |
| I2C write helper | `bbqX0kbd_i2cHelper.c` | 14–15 |

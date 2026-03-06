#pragma once

#include <stdbool.h>
#include <stdint.h>

void backlight_sync(void);
void backlight_init(void);

#ifdef BACKLIGHT_PERSIST
/* Load last saved backlight level from flash; use default_val if none valid. */
uint8_t backlight_load_saved(uint8_t default_val);
/* Schedule saving backlight level to flash (debounced). */
void backlight_schedule_save(uint8_t value);
#else
static inline uint8_t backlight_load_saved(uint8_t default_val) { (void)default_val; return 255; }
static inline void backlight_schedule_save(uint8_t value) { (void)value; }
#endif

#ifdef BACKLIGHT_IGNORE_HOST
/* Register key combo (Sym/Alt+0) to toggle backlight in firmware. Call after keyboard_init(). */
void backlight_register_toggle_combo(void);
/* True for a short period after key combo changed backlight; use to ignore delayed host packets. */
bool backlight_key_combo_cooldown_active(void);
#else
static inline void backlight_register_toggle_combo(void) { }
static inline bool backlight_key_combo_cooldown_active(void) { return false; }
#endif

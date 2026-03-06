#include "backlight.h"
#include "reg.h"

#include <hardware/pwm.h>
#include <pico/stdlib.h>

#ifdef BACKLIGHT_IGNORE_HOST
#include "keyboard.h"
#endif

#ifdef BACKLIGHT_PERSIST
#include <hardware/flash.h>
#include <hardware/sync.h>

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES	(2 * 1024 * 1024)
#endif

#define BACKLIGHT_PERSIST_OFFSET	(PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

#define BACKLIGHT_MAGIC		0x014B6C42u  /* "BkL" + version 1 */

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint8_t level;   /* 0 = off (user chose off), 1-255 = on at this level */
	uint8_t last_on; /* 1-255, level to restore when turning on from off */
	uint8_t reserved[250];
} backlight_persist_t;
#endif

void backlight_sync(void)
{
#ifdef BACKLIGHT_INVERT
	uint8_t inverted = 255 - reg_get_value(REG_ID_BKL);
	pwm_set_gpio_level(PIN_BKL, inverted * 0x80);
#else
	pwm_set_gpio_level(PIN_BKL, reg_get_value(REG_ID_BKL) * 0x80);
#endif
}

void backlight_init(void)
{
	gpio_set_function(PIN_BKL, GPIO_FUNC_PWM);

	const uint slice_num = pwm_gpio_to_slice_num(PIN_BKL);

	pwm_config config = pwm_get_default_config();
	pwm_init(slice_num, &config, true);

	backlight_sync();
}

#ifdef BACKLIGHT_IGNORE_HOST
/*
 * Backlight toggle: physical "Sym" (or Right Alt) + physical "0" key.
 * - Firmware keymap: that key is { '~', '0' } so we see '~' with Sym only, '0' with Alt.
 * - Also trigger on RELEASED when modifier is held, so one physical press still toggles
 *   if the host/modifier timing made PRESSED see no modifier yet (avoids needing 2 clicks).
 */
#define BACKLIGHT_COMBO_COOLDOWN_MS	500

static bool combo_key_down;
static bool toggled_this_press;
/* Set so cooldown is inactive at boot until first key combo. */
static uint32_t last_key_combo_ms = (uint32_t)-BACKLIGHT_COMBO_COOLDOWN_MS;

/* Map key to brightness level 1–9; 0 = not a level key. Physical number row gives letters (W,E,R,S,D,F,Z,X,C) or digits (1–9) depending on modifiers. */
static uint8_t backlight_key_to_level(char key)
{
	if (key >= '1' && key <= '9')
		return (uint8_t)(key - '0');
	/* Letters on keys that have 1–9 as alt: W=1,E=2,R=3,S=4,D=5,F=6,Z=7,X=8,C=9 */
	switch (key) {
	case 'W': return 1; case 'E': return 2; case 'R': return 3; case 'S': return 4;
	case 'D': return 5; case 'F': return 6; case 'Z': return 7; case 'X': return 8; case 'C': return 9;
	default: return 0;
	}
}

/* Filter: consume Sym+Shift+level key so it is never sent to the OS; apply brightness and return true. */
static bool backlight_level_filter_fn(char key, enum key_state state)
{
	if (state != KEY_STATE_PRESSED)
		return false;
	const bool sym_and_shift = keyboard_is_mod_on(KEY_MOD_ID_SYM) &&
		(keyboard_is_mod_on(KEY_MOD_ID_SHR) || keyboard_is_mod_on(KEY_MOD_ID_SHL));
	uint8_t level_idx = backlight_key_to_level(key);
	if (!sym_and_shift || level_idx == 0)
		return false;
	uint8_t level = (level_idx * 255u) / 9u;
	reg_set_value(REG_ID_BKL, level);
	backlight_sync();
	backlight_schedule_save(level);
	last_key_combo_ms = to_ms_since_boot(get_absolute_time());
	return true;  /* consumed: key not enqueued, OS never sees it */
}
static struct key_filter backlight_level_filter = { .filter = backlight_level_filter_fn, .next = NULL };

static void backlight_toggle_cb(char key, enum key_state state)
{
	/* Toggle: key with 0 on it — firmware sends '~' (Sym+key) or '0' (Alt+key) */
	if (key != '0' && key != '~')
		return;
	/* Sym key is often mapped to AltRight in kernel; accept either modifier */
	const bool mod_on = keyboard_is_mod_on(KEY_MOD_ID_SYM) || keyboard_is_mod_on(KEY_MOD_ID_ALT);

	if (state == KEY_STATE_PRESSED) {
		combo_key_down = true;
		toggled_this_press = false;
		if (mod_on) {
			uint8_t cur = reg_get_value(REG_ID_BKL);
			uint8_t next = cur ? 0 : backlight_load_last_on(255);  /* turn on = restore last-on level */
			reg_set_value(REG_ID_BKL, next);
			backlight_sync();
			backlight_schedule_save(next);  /* persist off (0) or on (level); last_on kept when saving 0 */
			last_key_combo_ms = to_ms_since_boot(get_absolute_time());
			toggled_this_press = true;
		}
	} else if (state == KEY_STATE_RELEASED) {
		if (combo_key_down && mod_on && !toggled_this_press) {
			uint8_t cur = reg_get_value(REG_ID_BKL);
			uint8_t next = cur ? 0 : backlight_load_last_on(255);
			reg_set_value(REG_ID_BKL, next);
			backlight_sync();
			backlight_schedule_save(next);
			last_key_combo_ms = to_ms_since_boot(get_absolute_time());
		}
		combo_key_down = false;
		toggled_this_press = false;
	}
}

bool backlight_key_combo_cooldown_active(void)
{
	uint32_t now = to_ms_since_boot(get_absolute_time());
	return (now - last_key_combo_ms) < BACKLIGHT_COMBO_COOLDOWN_MS;
}
static struct key_callback backlight_toggle_key_cb = { .func = backlight_toggle_cb };

void backlight_register_toggle_combo(void)
{
	keyboard_add_filter(&backlight_level_filter);  /* run first: consume brightness keys so OS never sees them */
	keyboard_add_key_callback(&backlight_toggle_key_cb);
}
#endif

#ifdef BACKLIGHT_PERSIST

/*
 * RAM-backed state: always up-to-date, survives debounce timing issues.
 * Flash is only written on a debounce timer and read once at boot.
 */
static uint8_t user_level;   /* 0 = off, 1-255 = on at that level */
static uint8_t user_last_on; /* 1-255, brightness to restore on toggle-on */
static bool persist_initialized;
static alarm_id_t save_alarm_id = -1;

static void persist_load_from_flash(void)
{
	const backlight_persist_t *p = (const backlight_persist_t *)(XIP_BASE + BACKLIGHT_PERSIST_OFFSET);
	if (p->magic == BACKLIGHT_MAGIC) {
		user_level = p->level;
		user_last_on = (p->last_on > 0) ? p->last_on : 255;
	} else {
		user_level = 255;
		user_last_on = 255;
	}
	persist_initialized = true;
}

static int64_t save_alarm_cb(alarm_id_t id, void *user_data)
{
	(void)id;
	(void)user_data;
	save_alarm_id = -1;

	backlight_persist_t block = { 0 };
	block.magic = BACKLIGHT_MAGIC;
	block.level = user_level;
	block.last_on = user_last_on;

	uint32_t irq = save_and_disable_interrupts();
	flash_range_erase(BACKLIGHT_PERSIST_OFFSET, FLASH_SECTOR_SIZE);
	flash_range_program(BACKLIGHT_PERSIST_OFFSET, (const uint8_t *)&block, FLASH_PAGE_SIZE);
	restore_interrupts(irq);

	return 0;  /* one-shot */
}

uint8_t backlight_load_saved(uint8_t default_val)
{
	if (!persist_initialized)
		persist_load_from_flash();
	(void)default_val;
	return user_level;  /* 0 = user chose off, 1-255 = on at that level */
}

uint8_t backlight_load_last_on(uint8_t default_val)
{
	if (!persist_initialized)
		persist_load_from_flash();
	return (user_last_on > 0) ? user_last_on : default_val;
}

void backlight_schedule_save(uint8_t value)
{
	if (!persist_initialized)
		persist_load_from_flash();
	if (value > 0) {
		user_level = value;
		user_last_on = value;
	} else {
		user_level = 0;
		/* user_last_on stays unchanged so toggle-on restores it */
	}
	if (save_alarm_id >= 0)
		cancel_alarm(save_alarm_id);
	save_alarm_id = add_alarm_in_ms(2000, save_alarm_cb, NULL, false);
}

#endif /* BACKLIGHT_PERSIST */

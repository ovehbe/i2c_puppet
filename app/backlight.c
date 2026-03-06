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
	uint8_t level;
	uint8_t reserved[251];
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

static void backlight_toggle_cb(char key, enum key_state state)
{
	/* Key with 0 on it: firmware sends '~' (Sym+key) or '0' (Alt+key) */
	if (key != '0' && key != '~')
		return;
	/* Sym key is often mapped to AltRight in kernel; accept either modifier */
	const bool mod_on = keyboard_is_mod_on(KEY_MOD_ID_SYM) || keyboard_is_mod_on(KEY_MOD_ID_ALT);

	if (state == KEY_STATE_PRESSED) {
		combo_key_down = true;
		toggled_this_press = false;
		if (mod_on) {
			uint8_t cur = reg_get_value(REG_ID_BKL);
			uint8_t next = cur ? 0 : 255;
			reg_set_value(REG_ID_BKL, next);
			backlight_sync();
			backlight_schedule_save(next);
			last_key_combo_ms = to_ms_since_boot(get_absolute_time());
			toggled_this_press = true;
		}
	} else if (state == KEY_STATE_RELEASED) {
		if (combo_key_down && mod_on && !toggled_this_press) {
			uint8_t cur = reg_get_value(REG_ID_BKL);
			uint8_t next = cur ? 0 : 255;
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
	keyboard_add_key_callback(&backlight_toggle_key_cb);
}
#endif

#ifdef BACKLIGHT_PERSIST

static uint8_t save_pending = 0xFF;
static alarm_id_t save_alarm_id = -1;

static int64_t save_alarm_cb(alarm_id_t id, void *user_data)
{
	(void)id;
	(void)user_data;

	uint8_t level = save_pending;
	save_pending = 0xFF;  /* mark none pending */
	save_alarm_id = -1;

	backlight_persist_t block = { 0 };
	block.magic = BACKLIGHT_MAGIC;
	block.level = level;

	uint32_t irq = save_and_disable_interrupts();
	flash_range_erase(BACKLIGHT_PERSIST_OFFSET, FLASH_SECTOR_SIZE);
	flash_range_program(BACKLIGHT_PERSIST_OFFSET, (const uint8_t *)&block, FLASH_PAGE_SIZE);
	restore_interrupts(irq);

	return 0;  /* one-shot */
}

uint8_t backlight_load_saved(uint8_t default_val)
{
	const backlight_persist_t *p = (const backlight_persist_t *)(XIP_BASE + BACKLIGHT_PERSIST_OFFSET);
	if (p->magic != BACKLIGHT_MAGIC)
		return default_val;
	return p->level;
}

void backlight_schedule_save(uint8_t value)
{
	save_pending = value;
	if (save_alarm_id >= 0)
		cancel_alarm(save_alarm_id);
	save_alarm_id = add_alarm_in_ms(2000, save_alarm_cb, NULL, false);
}

#endif /* BACKLIGHT_PERSIST */

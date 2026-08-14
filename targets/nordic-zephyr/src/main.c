#include "od_board.h"
#include "od_log.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_display.h"
#include "od_runtime_types.h"
#include "od_watchdog_app.h"

#include <stdio.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif

static void idle_delay_ms(uint32_t delay_ms)
{
	const uint32_t chunk_ms = 1000u;
	uint32_t remaining = delay_ms;

	while (remaining > 0u) {
		uint32_t step = (remaining > chunk_ms) ? chunk_ms : remaining;

		/* The idle wait is chunked at one second, so feeding here rather than only in the
		 * caller is what keeps a configured sleep_timeout_ms of minutes from looking like
		 * a hang. Reaching this line proves the thread is running, which is the whole
		 * requirement for a feed site. */
		od_watchdog_app_service();
		opendisplay_ble_process();
		k_msleep(step);
		remaining -= step;
	}
}

int main(void)
{
	const struct od_config *cfg;
	uint32_t ticks = 0;

	/* Compatibility hook before the first application record. */
	od_log_init();
#if defined(OD_DEBUG_BUILD)
	od_log_info("OpenDisplay %s DEBUG starting", od_board_name());
#else
	od_log_info("OpenDisplay %s starting", od_board_name());
#endif
	/* Before anything can touch the panel: this decides whether the previous run ended in a
	 * watchdog reset, and whether three of them in a row mean this boot must skip the panel
	 * entirely. Arming comes after, so a hang inside boot_init cannot be masked by a reset. */
	od_watchdog_app_boot();
	od_watchdog_app_arm();

	od_board_early_init();
	opendisplay_ble_init();
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	/* Confirm running image so MCUboot will not revert after OTA. */
	(void)boot_write_img_confirmed();
#endif

	while (1) {
		od_watchdog_app_service();
		cfg = opendisplay_get_global_config();

		if (opendisplay_ble_is_connected()) {
			opendisplay_ble_process();
#if !defined(OD_LOW_POWER_QUIET)
			if ((ticks++ % 100u) == 0u) {
				od_log_info("OpenDisplay alive uptime=%u ms", k_uptime_get_32());
			}
#else
			ticks++;
#endif
			k_msleep(10);
			continue;
		}

		/* Matches nRF52840 Firmware: MSD refreshes once per sleep_timeout_ms
		 * idle cycle; without a configured timeout there is no periodic MSD
		 * update (buttons and adv restarts still refresh it). */
		if (cfg != NULL && cfg->loaded && cfg->power_option.sleep_timeout_ms > 0u) {
			idle_delay_ms(cfg->power_option.sleep_timeout_ms);
			opendisplay_ble_update_msd(true);
		} else {
			idle_delay_ms(500u);
		}

#if !defined(OD_LOW_POWER_QUIET)
		if ((ticks++ % 10u) == 0u) {
			od_log_info("OpenDisplay alive uptime=%u ms", k_uptime_get_32());
		}
#else
		ticks++;
#endif
	}
	return 0;
}
